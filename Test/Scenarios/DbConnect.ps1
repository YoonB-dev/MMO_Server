# DB 연결: 서버가 시작할 때 config.ini의 DB에 접속하고, 확인 쿼리(SELECT DATABASE(), VERSION())까지 도는지
#   DB가 없거나 MySQL이 꺼져 있으면 서버가 이유를 찍고 종료 -> Start-GameServer가 그 로그와 함께 실패
Start-GameServer

Assert-LogContains -Name server -Pattern '\[DB\] connected: mmo_db'
Assert-ServerAlive
