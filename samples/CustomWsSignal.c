#include "CustomWsSignal.h"
#include <libwebsockets.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#define MAX_JSON 65536

static PSampleConfiguration gCfg = NULL;
static struct lws* gWsi = NULL;
static char gRoom[128];
static char gUid[128];
static PRtcPeerConnection gPc = NULL;

/* WebSocket 分片接收缓冲 */
static char rxBuf[MAX_JSON];
static size_t rxLen = 0;

static STATUS wsSendDyn(struct lws* wsi, const char* fmt, va_list apOrig) {
    va_list ap; va_copy(ap, apOrig);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed <= 0 || needed >= MAX_JSON) {
        return STATUS_INTERNAL_ERROR;
    }
    size_t size = LWS_PRE + needed + 1;
    unsigned char* buf = (unsigned char*) malloc(size);
    if (!buf) return STATUS_NOT_ENOUGH_MEMORY;
    unsigned char* p = buf + LWS_PRE;
    va_copy(ap, apOrig);
    vsnprintf((char*) p, needed + 1, fmt, ap);
    va_end(ap);
    int wrote = lws_write(wsi, p, needed, LWS_WRITE_TEXT);
    free(buf);
    return wrote < needed ? STATUS_INTERNAL_ERROR : STATUS_SUCCESS;
}

static STATUS wsSend(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    STATUS ret = wsSendDyn(gWsi, fmt, ap);
    va_end(ap);
    return ret;
}

static VOID onIce(UINT64 customData, PCHAR candJson) {
    (void)customData;
    if (candJson == NULL) {
        // gathering 完成信号，忽略
        return;
    }
    /* candJson 形如 {"candidate":"candidate:0 1 UDP ...","sdpMid":"0","sdpMLineIndex":0}，
     * 解析出真正的 SDP candidate 字符串后再发送，避免嵌套 JSON 造成解析失败。*/
    char cand[1500] = {0};
    const char *s = strstr(candJson, "\"candidate\":\"");
    if (s != NULL) {
        s += strlen("\"candidate\":\"");
        const char *e = strchr(s, '"');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len >= sizeof(cand)) len = sizeof(cand) - 1;
        memcpy(cand, s, len);
        cand[len] = '\0';
    } else {
        STRNCPY(cand, candJson, sizeof(cand));
    }
    wsSend("{\"type\":\"ICE_CANDIDATE\",\"room\":\"%s\",\"uid\":\"%s\",\"candidate\":\"%s\"}",
           gRoom, gUid, cand);
}

static void jsonEscape(const char* src, char* dst, size_t max) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < max; i++) {
        char c = src[i];
        switch (c) {
        case '"': if (j + 2 < max) { dst[j++] = '\\'; dst[j++] = '"'; } break;
        case '\\': if (j + 2 < max) { dst[j++] = '\\'; dst[j++] = '\\'; } break;
        case '\n': if (j + 2 < max) { dst[j++] = '\\'; dst[j++] = 'n'; } break;
        case '\r': if (j + 2 < max) { dst[j++] = '\\'; dst[j++] = 'r'; } break;
        default: dst[j++] = c; break;
        }
    }
    dst[j] = '\0';
}

static void jsonUnescape(char* s) {
    char* w = s; // write pointer
    for (char* r = s; *r != '\0'; r++) {
        if (*r == '\\' && *(r+1) != '\0') {
            r++;
            switch(*r) {
            case 'n': *w++ = '\n'; break;
            case 'r': *w++ = '\r'; break;
            case 't': *w++ = '\t'; break;
            case '"': *w++ = '"'; break;
            case '\\': *w++ = '\\'; break;
            default: *w++ = *r; break;
            }
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static BOOL extractJsonString(const char* msg, const char* key, char* out, size_t max) {
    char pattern[32];
    SNPRINTF(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(msg, pattern);
    if (p == NULL) return FALSE;
    p += strlen(pattern);
    const char* q = p;
    while (*q) {
        if (*q == '"' && *(q-1) != '\\') break;
        q++;
    }
    size_t len = q - p;
    if (len >= max) len = max - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    jsonUnescape(out);
    return TRUE;
}

static void parseAndHandle(const char* msg) {
    if (strstr(msg, "\"SDP_ANSWER\"")) {
        char sdp[30000];
        if (!extractJsonString(msg, "sdp", sdp, sizeof(sdp))) return;
        RtcSessionDescriptionInit ans; MEMSET(&ans, 0, SIZEOF(ans));
        ans.type = SDP_TYPE_ANSWER; STRNCPY(ans.sdp, sdp, MAX_SESSION_DESCRIPTION_INIT_SDP_LEN);
        setRemoteDescription(gPc, &ans);
    } else if (strstr(msg, "\"ICE_CANDIDATE\"")) {
        char cand[1600];
        if (!extractJsonString(msg, "candidate", cand, sizeof(cand))) return;
        addIceCandidate(gPc, cand);
    }
}

static int wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                      void* user, void* in, size_t len) {
    (void)user;
    switch(reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED: {
        gWsi = wsi;
        wsSend("{\"type\":\"JOIN\",\"room\":\"%s\",\"uid\":\"%s\",\"role\":\"kvs\"}", gRoom, gUid);
        RtcSessionDescriptionInit offer; MEMSET(&offer, 0, SIZEOF(offer));
        createOffer(gPc, &offer);
        setLocalDescription(gPc, &offer);
        size_t escLen = STRLEN(offer.sdp)*2 + 1;
        char* esc = (char*) malloc(escLen);
        if (esc == NULL) {
            DLOGE("[Master] malloc esc failed");
            break; }
        jsonEscape(offer.sdp, esc, escLen);
        wsSend("{\"type\":\"SDP_OFFER\",\"room\":\"%s\",\"uid\":\"%s\",\"sdp\":\"%s\"}", gRoom, gUid, esc);
        free(esc);
        break; }
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        /* 累积分片数据 */
        if (rxLen + len < sizeof(rxBuf)) {
            MEMCPY(rxBuf + rxLen, in, len);
            rxLen += len;
        }
        if (lws_is_final_fragment(wsi)) {
            rxBuf[rxLen] = '\0';
            parseAndHandle(rxBuf);
            rxLen = 0;
        }
        break; }
    case LWS_CALLBACK_CLIENT_CLOSED:
        ATOMIC_STORE_BOOL(&gCfg->appTerminateFlag, TRUE);
        break;
    default:
        break;
    }
    return 0;
}

STATUS runWsSignalingMaster(PRtcPeerConnection pc, PSampleConfiguration cfg,
                            const char* wsUrl, const char* room, const char* uid) {
    gPc = pc; gCfg = cfg;
    STRNCPY(gRoom, room, sizeof(gRoom)); STRNCPY(gUid, uid, sizeof(gUid));
    peerConnectionOnIceCandidate(gPc, 0, onIce);

    struct lws_protocols prot[] = {{"ws", wsCallback, 0, MAX_JSON, 0, NULL, 0}, {NULL, NULL, 0, 0, 0, NULL, 0}};
    struct lws_context_creation_info info; MEMSET(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN; info.protocols = prot;
    struct lws_context* ctx = lws_create_context(&info);

    int port = 0; int ssl = 0;
    const char *adshost = NULL, *pathptr = NULL, *proto = NULL;
    char uri[256]; STRNCPY(uri, wsUrl, sizeof(uri));
    if (lws_parse_uri(uri, &proto, &adshost, &port, &pathptr)) {
        return STATUS_INVALID_ARG;
    }

    struct lws_client_connect_info cc; MEMSET(&cc, 0, sizeof(cc));
    cc.context = ctx; cc.address = adshost; cc.port = port; cc.path = pathptr; cc.protocol = "ws";
    gWsi = lws_client_connect_via_info(&cc);

    while(!ATOMIC_LOAD_BOOL(&cfg->appTerminateFlag)) {
        lws_service(ctx, 100);
    }
    lws_context_destroy(ctx);
    return STATUS_SUCCESS;
} 