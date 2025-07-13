#pragma once
#include "Samples.h"
#ifdef __cplusplus
extern "C" {
#endif

/*
 * 运行自定义 WebSocket 信令循环，阻塞直到会话结束
 *  - cfg:  样例配置指针，里面包含 PeerConnection、terminateFlag 等
 *  - wsUrl: 信令服务器地址，例如 "ws://127.0.0.1:8080/ws"
 *  - room:  房间名
 *  - uid:   本端唯一标识，例如 "master"
 */
STATUS runWsSignalingMaster(PRtcPeerConnection pc,
                            PSampleConfiguration cfg,
                            const char* wsUrl,
                            const char* room,
                            const char* uid);

STATUS runWsSignalingViewer(PRtcPeerConnection pc,
                              PSampleConfiguration cfg,
                              const char* wsUrl,
                              const char* room,
                              const char* uid);

#ifdef __cplusplus
}
#endif 