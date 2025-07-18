package main

import (
    "crypto/tls"
    "encoding/json"
    "flag"
    "fmt"
    "log"
    "net/http"
    "strings"
    "sync"

    "github.com/gorilla/websocket"
    "github.com/pion/webrtc/v3"
)

/**************** 信令结构 ****************/

type Signal struct {
    Type      string `json:"type"`
    Room      string `json:"room,omitempty"`
    UID       string `json:"uid,omitempty"`
    To        string `json:"to,omitempty"`
    SDP       string `json:"sdp,omitempty"`
    Candidate string `json:"candidate,omitempty"`
    Role      string `json:"role,omitempty"`
}

/**************** 房间与 Peer ****************/

type Peer struct {
    uid         string
    conn        *websocket.Conn
    send        chan []byte
    pc          *webrtc.PeerConnection
    isKvsMaster bool
    room        *Room
}

type Room struct {
    name        string
    master      *Peer
    viewers     map[string]*Peer
    relayTracks map[string]*webrtc.TrackLocalStaticRTP
    lock        sync.RWMutex
}

var (
    upgrader = websocket.Upgrader{CheckOrigin: func(r *http.Request) bool { return true }}
    rooms    = map[string]*Room{}
    roomsLk  sync.RWMutex
    api      *webrtc.API
)

/**************** WebSocket 入口 ****************/

func wsHandler(w http.ResponseWriter, r *http.Request) {
    roomName := r.URL.Query().Get("room")
    uid := r.URL.Query().Get("uid")
    if roomName == "" || uid == "" {
        http.Error(w, "need room & uid", http.StatusBadRequest)
        return
    }
    c, err := upgrader.Upgrade(w, r, nil)
    if err != nil {
        return
    }
    room := getRoom(roomName)
    p := &Peer{uid: uid, conn: c, send: make(chan []byte, 256), room: room}

    go p.writeLoop()
    p.readLoop()
}

/**************** 房间管理 ****************/

func getRoom(name string) *Room {
    roomsLk.Lock()
    defer roomsLk.Unlock()
    if r, ok := rooms[name]; ok {
        return r
    }
    r := &Room{name: name, viewers: map[string]*Peer{}, relayTracks: map[string]*webrtc.TrackLocalStaticRTP{}}
    rooms[name] = r
    return r
}

/**************** Peer I/O ****************/

func (p *Peer) writeLoop() {
    for msg := range p.send {
        log.Printf("WS Send To: %s %s", p.uid, msg)
        if err := p.conn.WriteMessage(websocket.TextMessage, msg); err != nil {
            log.Printf("[ERR] WS write to %s: %v", p.uid, err)
        }
    }
}

func (p *Peer) readLoop() {
    defer func() {
        p.conn.Close()
        p.closePeer()
    }()

    for {
        _, data, err := p.conn.ReadMessage()
        if err != nil {
            return
        }
        log.Printf("WS Recv From: %s %s \n", p.uid, data)
        var sig Signal
        if err := json.Unmarshal(data, &sig); err != nil {
            log.Println("json error:", err.Error())
            continue
        }
        log.Println("begin switch")
        log.Println(sig)
        switch sig.Type {
        case "JOIN":
            p.handleJoin(sig)
        case "SDP_OFFER":
            p.handleOffer(sig)
        case "SDP_ANSWER":
            p.handleAnswer(sig)
        case "ICE_CANDIDATE":
            p.handleCandidate(sig)
        case "LEAVE":
            return
        }
    }
}

/******** JOIN ********/

func (p *Peer) handleJoin(sig Signal) {
    if strings.ToLower(sig.Role) == "kvs" {
        p.isKvsMaster = true
        if err := p.room.setMaster(p); err != nil {
            log.Printf("[WARN] Room %s already has a master, rejecting new master %s", p.room.name, p.uid)
            return
        }
        // 如果 JOIN 消息附带 SDP，则视为 master 的初始 Offer
        if sig.SDP != "" {
            log.Printf("recv sdp from : %s", sig.UID)
            p.handleOffer(sig)
        }
    } else {
        p.room.addViewer(p)
    }
}

/******** SDP 处理 ********/

func (p *Peer) handleOffer(sig Signal) {
    log.Printf("handleOffer 11111")
    if p.isKvsMaster {
        log.Printf("handleOffer 222")
        // KVS master -> 我们 viewer
        p.pc = newPeerConnection()
        log.Printf("handleOffer 333")
        p.pc.OnICECandidate(func(c *webrtc.ICECandidate) {
            if c == nil {
                log.Printf("handleOffer 4444")
                return
            }
            payload, _ := json.Marshal(Signal{
                Type:      "ICE_CANDIDATE",
                Room:      sig.Room,
                UID:       p.uid,
                Candidate: c.ToJSON().Candidate,
            })
            p.send <- payload
        })
        log.Printf("handleOffer 55555")
        p.pc.OnTrack(func(remote *webrtc.TrackRemote, recv *webrtc.RTPReceiver) {
            log.Printf("handleOffer 66666")
            _ = recv
            key := fmt.Sprintf("%s_%d", remote.Kind().String(), remote.SSRC())
            rt, _ := p.room.getOrCreateRelay(remote, key)
            fmt.Printf(">>> 新 Track: SSRC=%d PT=%d kind=%s\n", remote.SSRC(), remote.PayloadType(), remote.Kind().String())
            pktCnt := 0
            byteCnt := 0
            // 读 remote RTP 并写到 relay track
            for {
                pkt, _, err := remote.ReadRTP()
                if err != nil {
                    return
                }
                pktCnt++
                byteCnt += len(pkt.Payload)
                if pktCnt%50 == 0 {
                    fmt.Printf("[RTP] SSRC=%d Seq=%d TS=%d 累计=%d包/%d字节\n", pkt.SSRC, pkt.SequenceNumber, pkt.Timestamp, pktCnt, byteCnt)
                }
                _ = rt.WriteRTP(pkt)
            }
        })
        log.Printf("111111111")
        if err := p.pc.SetRemoteDescription(webrtc.SessionDescription{
            Type: webrtc.SDPTypeOffer, SDP: sig.SDP}); err != nil {
            log.Printf("[ERR] SetRemoteDescription: %v", err)
            return
        }
        log.Printf("22222222")
        answer, err := p.pc.CreateAnswer(nil)
        if err != nil {
            log.Printf("[ERR] CreateAnswer: %v", err)
            return
        }
        log.Printf("333333333")
        if err := p.pc.SetLocalDescription(answer); err != nil {
            log.Printf("[ERR] SetLocalDescription: %v", err)
            return
        }
        log.Printf("Send SDP_ANSWER to %s (len=%d)", p.uid, len(answer.SDP))
        payload, _ := json.Marshal(Signal{Type: "SDP_ANSWER", Room: sig.Room, UID: p.uid, SDP: answer.SDP})
        p.send <- payload
    } else {
        // viewer 收到 offer（正常流程由 viewer 发 offer，这里忽略）
    }
}

func (p *Peer) handleAnswer(sig Signal) {
    if !p.isKvsMaster && p.pc != nil {
        _ = p.pc.SetRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeAnswer, SDP: sig.SDP})
    }
}

/******** ICE candidate ********/

func (p *Peer) handleCandidate(sig Signal) {
    if p.pc == nil {
        return
    }
    _ = p.pc.AddICECandidate(webrtc.ICECandidateInit{Candidate: sig.Candidate})
}

/******** Room helpers ********/

func (r *Room) setMaster(p *Peer) error {
    r.lock.Lock(); 
    defer r.lock.Unlock()
    if r.master != nil && r.master.conn != nil {
        // 先广播 BUSY，或直接关闭旧连接
        r.lock.Unlock();
        r.master.closePeer()
        r.lock.Lock()
    }
    r.master = p
    return nil
}

func (r *Room) addViewer(p *Peer) {
    r.lock.Lock()
    r.viewers[p.uid] = p
    r.lock.Unlock()

    // 若已有可用媒体轨，则立即向 viewer 发送 Offer；否则等 track 到来时 getOrCreateRelay 再触发
    if len(r.relayTracks) > 0 {
        p.sendOffer(r)
    }
}

func (p *Peer) closePeer() {
    r := p.room
    r.lock.Lock()
    defer r.lock.Unlock()

    // ------ 当当前 Peer 是 master 且正在离开 ------
    if p.isKvsMaster && r.master == p {
        // 清空 master 引用
        r.master = nil

        // 清空房间内现有的中继 track，确保不会把过期的 track 继续推送给 viewer
        r.relayTracks = map[string]*webrtc.TrackLocalStaticRTP{}

        // 遍历所有 viewer：只关闭其 PeerConnection，保留 WebSocket send 通道
        for _, v := range r.viewers {
            if v.pc != nil {
                _ = v.pc.Close()
                v.pc = nil
            }
            // 如有需要，可向 viewer 发送自定义事件通知流已重置
            // payload, _ := json.Marshal(Signal{Type: "STREAM_RESET", Room: r.name})
            // v.send <- payload
        }

        // 关闭 master 自身的资源
        if p.pc != nil {
            _ = p.pc.Close()
        }
        close(p.send)
        return
    }

    // ------ 普通 viewer 离开 ------
    if !p.isKvsMaster {
        delete(r.viewers, p.uid)
    }
    if p.pc != nil {
        _ = p.pc.Close()
    }
    close(p.send)
}

/******** Track relay ********/

func (r *Room) getOrCreateRelay(remote *webrtc.TrackRemote, key string) (*webrtc.TrackLocalStaticRTP, error) {
    r.lock.Lock()
    defer r.lock.Unlock()

    if t, ok := r.relayTracks[key]; ok {
        return t, nil
    }
    rt, err := webrtc.NewTrackLocalStaticRTP(remote.Codec().RTPCodecCapability, remote.ID(), remote.StreamID())
    if err != nil {
        return nil, err
    }
    r.relayTracks[key] = rt
    // 将 relay track 添加到已有 viewer 的 PC
    for _, v := range r.viewers {
        // 在 getOrCreateRelay 里，viewer 已有 pc 的情况
        if v.pc != nil {
            _, _ = v.pc.AddTrack(rt)

            // 重新协商，把新 track 告知 viewer
            offer, _ := v.pc.CreateOffer(nil)
            _ = v.pc.SetLocalDescription(offer)
            payload, _ := json.Marshal(Signal{
                Type: "SDP_OFFER",
                Room: r.name,
                UID:  v.uid,
                SDP:  offer.SDP,
            })
            v.send <- payload
        } else {
            v.sendOffer(r) // 首次建立连接的分支保持不变
        }
    }
    return rt, nil
}

/******** viewer 主动发送 offer ********/

func (p *Peer) sendOffer(r *Room) {
    p.pc = newPeerConnection()
    p.pc.OnICECandidate(func(c *webrtc.ICECandidate) {
        if c == nil {
            return
        }
        payload, _ := json.Marshal(Signal{Type: "ICE_CANDIDATE", Room: r.name, UID: p.uid, Candidate: c.ToJSON().Candidate})
        p.send <- payload
    })
    for _, rt := range r.relayTracks {
        _, _ = p.pc.AddTrack(rt)
    }
    offer, _ := p.pc.CreateOffer(nil)
    _ = p.pc.SetLocalDescription(offer)
    payload, _ := json.Marshal(Signal{Type: "SDP_OFFER", Room: r.name, UID: p.uid, SDP: offer.SDP})
    p.send <- payload
}

/******** PeerConnection 创建 ********/

func newPeerConnection() *webrtc.PeerConnection {
    conf := webrtc.Configuration{ICEServers: []webrtc.ICEServer{{URLs: []string{"stun:stun.l.google.com:19302"}}}}
    pc, err := api.NewPeerConnection(conf)
    if err != nil {
        panic(err)
    }
    return pc
}

/******** main ********/

func main() {
    me := &webrtc.MediaEngine{}
    _ = me.RegisterDefaultCodecs()
    api = webrtc.NewAPI(webrtc.WithMediaEngine(me))

    // 证书参数
    certPath := flag.String("cert", "certs/server.crt", "TLS cert")
    keyPath := flag.String("key", "certs/server.key", "TLS key")
    addrHTTP := flag.String("http", ":8080", "HTTP listen")
    addrHTTPS := flag.String("https", ":443", "HTTPS listen")
    flag.Parse()

    http.HandleFunc("/ws", wsHandler)
    // 静态文件目录 static
    http.Handle("/", http.FileServer(http.Dir("./static")))

    // HTTPS goroutine
    go func() {
        if _, err := tls.LoadX509KeyPair(*certPath, *keyPath); err != nil {
            log.Printf("[WARN] unable to load cert/key (%v), skip HTTPS", err)
            return
        }
        log.Printf("HTTPS/WebSocket Secure listening on %s", *addrHTTPS)
        if err := http.ListenAndServeTLS(*addrHTTPS, *certPath, *keyPath, nil); err != nil {
            log.Fatalf("HTTPS failed: %v", err)
        }
    }()

    log.Printf("HTTP/WebSocket listening on %s", *addrHTTP)
    log.Fatal(http.ListenAndServe(*addrHTTP, nil))
} 