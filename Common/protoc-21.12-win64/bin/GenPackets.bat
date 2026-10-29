@echo off
REM ============================================================
REM  .proto -> *.pb.h/.cc + ClientPacketHandler.h / ServerPacketHandler.h
REM  GameServer 빌드 전 이벤트에서 호출 (DummyClient는 GameServer 다음에 빌드됨)
REM  내용이 바뀐 파일만 복사해서, 패킷이 그대로면 불필요한 재컴파일이 없게 함
REM ============================================================
setlocal
pushd %~dp0

"%~dp0protoc.exe" -I=./ --cpp_out=./ ./Enum.proto ./Struct.proto ./Protocol.proto || goto :error

"%~dp0GenPackets.exe" --path=./Protocol.proto --output=ClientPacketHandler --recv=C_ --send=S_ > nul 2>&1 || goto :error
"%~dp0GenPackets.exe" --path=./Protocol.proto --output=ServerPacketHandler --recv=S_ --send=C_ > nul 2>&1 || goto :error

for %%F in (*.pb.h *.pb.cc) do (
	call :copy_if_changed %%F ..\..\..\GameServer
	call :copy_if_changed %%F ..\..\..\DummyClient
)
call :copy_if_changed ClientPacketHandler.h ..\..\..\GameServer
call :copy_if_changed ServerPacketHandler.h ..\..\..\DummyClient

del /F /Q *.pb.* *PacketHandler.h
popd
exit /b 0

:copy_if_changed
fc /b "%~1" "%~2\%~nx1" > nul 2>&1 || copy /Y "%~1" "%~2\" > nul
exit /b 0

:error
echo [GenPackets] packet generation failed (run GenPackets.bat in this folder to see details)
del /F /Q *.pb.* *PacketHandler.h > nul 2>&1
popd
exit /b 1
