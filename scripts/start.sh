# 终端或启动脚本中执行
export AWS_ACCESS_KEY_ID=dummyAccessKey
export AWS_SECRET_ACCESS_KEY=dummySecretKey
export AWS_KVS_LOG_LEVEL=1

cd ../pion_server
go run bridge.go -cert certs/server.crt -key certs/server.key -http :8080 -https :443

./kvsWebrtcClientMaster demo 'ws://127.0.0.1:8080/ws?room=demo&uid=master&role=kvs'

./kvsWebrtcClientViewer demo 'ws://127.0.0.1:8080/ws?room=demo&uid=viewer1'
