# 终端或启动脚本中执行
export AWS_ACCESS_KEY_ID=dummyAccessKey
export AWS_SECRET_ACCESS_KEY=dummySecretKey


./kvsWebrtcClientMaster demo ws://127.0.0.1:8080/ws?room=demo&uid=master

./kvsWebrtcClientViewer demo 'ws://127.0.0.1:8080/ws?room=demo&uid=viewer1'