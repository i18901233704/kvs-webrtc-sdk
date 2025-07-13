#include "CustomWsSignal.h"
#include <libwebsockets.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#define MAX_JSON 65536

// 提前声明全局静态变量，供后续函数引用
static PSampleConfiguration vCfg = NULL;
static struct lws* vWsi = NULL;
static char vRoom[128];
static char vUid[128];
static PRtcPeerConnection vPc = NULL;

/* WebSocket 分片接收缓冲 */
static char rxBuf[MAX_JSON];
static size_t rxLen = 0;

static STATUS wsSendDynV(struct lws* wsi, const char* fmt, va_list apOrig) {
    va_list ap; va_copy(ap, apOrig);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed <= 0 || needed >= MAX_JSON) return STATUS_INTERNAL_ERROR;
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

static STATUS wsSendV(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    STATUS ret = wsSendDynV(vWsi, fmt, ap);
    va_end(ap);
    return ret;
}

static VOID onIceV(UINT64, PCHAR candJson) {
    if (candJson == NULL) {
        return; // gathering 完成
    }
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
    wsSendV("{\"type\":\"ICE_CANDIDATE\",\"room\":\"%s\",\"uid\":\"%s\",\"candidate\":\"%s\"}",
            vRoom, vUid, cand);
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
    char* w = s;
    for (char* r = s; *r; r++) {
        if (*r == '\\' && *(r+1)) {
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

static void handleMsgV(const char* msg) {
    if (strstr(msg, "\"SDP_OFFER\"")) {
        char sdp[30000]; if (!extractJsonString(msg, "sdp", sdp, sizeof(sdp))) return;
        /* 同时用 fprintf 直写 stderr，避免日志等级过滤 */
        fprintf(stderr, "\n[Viewer] SDP_OFFER length=%zu\n%.*s\n", STRLEN(sdp), 300, sdp);
        DLOGI("[Viewer] SDP_OFFER length: %u", (UINT32) STRLEN(sdp));
        DLOGI("[Viewer] SDP_OFFER head:\n%.*s", 300, sdp);

        RtcSessionDescriptionInit offer; MEMSET(&offer, 0, SIZEOF(offer));
        offer.type = SDP_TYPE_OFFER; STRNCPY(offer.sdp, sdp, MAX_SESSION_DESCRIPTION_INIT_SDP_LEN);
        setRemoteDescription(vPc, &offer);
        RtcSessionDescriptionInit answer; MEMSET(&answer, 0, SIZEOF(answer));
        createAnswer(vPc, &answer);
        setLocalDescription(vPc, &answer);
        /* 动态分配转义缓冲，长度 = 原 SD P*2 +1 以防转义后变长 */
        size_t escLen = STRLEN(answer.sdp) * 2 + 1;
        char* esc = (char*) malloc(escLen);
        if (esc == NULL) {
            DLOGE("[Viewer] malloc failed for esc");
            return;
        }
        jsonEscape(answer.sdp, esc, escLen);
        wsSendV("{\"type\":\"SDP_ANSWER\",\"room\":\"%s\",\"uid\":\"%s\",\"sdp\":\"%s\"}", vRoom, vUid, esc);
        free(esc);
    } else if (strstr(msg, "\"ICE_CANDIDATE\"")) {
        char cand[1600]; if (!extractJsonString(msg, "candidate", cand, sizeof(cand))) return;
        addIceCandidate(vPc, cand);
    }
}

static int wsCbV(struct lws* wsi, enum lws_callback_reasons reason,
                 void* user, void* in, size_t len) {
    (void)user;
    switch(reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        vWsi = wsi;
        wsSendV("{\"type\":\"JOIN\",\"room\":\"%s\",\"uid\":\"%s\"}", vRoom, vUid);
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (rxLen + len < sizeof(rxBuf)) {
            MEMCPY(rxBuf + rxLen, in, len);
            rxLen += len;
        }
        if (lws_is_final_fragment(wsi)) {
            rxBuf[rxLen] = '\0';
            handleMsgV(rxBuf);
            rxLen = 0;
        }
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        ATOMIC_STORE_BOOL(&vCfg->appTerminateFlag, TRUE);
        break;
    default:
        break;
    }
    return 0;
}

STATUS runWsSignalingViewer(PRtcPeerConnection pc, PSampleConfiguration cfg,
                            const char* wsUrl, const char* room, const char* uid) {
    vPc = pc; vCfg = cfg;
    STRNCPY(vRoom, room, sizeof(vRoom)); STRNCPY(vUid, uid, sizeof(vUid));
    peerConnectionOnIceCandidate(vPc, 0, onIceV);

    struct lws_protocols prot[] = {{"ws", wsCbV, 0, MAX_JSON, 0, NULL, 0}, {NULL, NULL, 0, 0, 0, NULL, 0}};
    struct lws_context_creation_info info; MEMSET(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN; info.protocols = prot;
    struct lws_context* ctx = lws_create_context(&info);

    const char *proto=NULL,*adshost=NULL,*pathptr=NULL; int port=0, ssl=0;
    char uri[256]; STRNCPY(uri, wsUrl, sizeof(uri));
    if (lws_parse_uri(uri, &proto, &adshost, &port, &pathptr)) return STATUS_INVALID_ARG;

    struct lws_client_connect_info cc; MEMSET(&cc, 0, sizeof(cc));
    cc.context=ctx; cc.address=adshost; cc.port=port; cc.path=pathptr; cc.protocol="ws";
    vWsi = lws_client_connect_via_info(&cc);

    while(!ATOMIC_LOAD_BOOL(&cfg->appTerminateFlag)) {
        lws_service(ctx, 100);
    }
    lws_context_destroy(ctx);
    return STATUS_SUCCESS;
} 