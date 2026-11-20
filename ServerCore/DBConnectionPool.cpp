#include "pch.h"
#include "DBConnectionPool.h"

DBConnectionPool::DBConnectionPool()
{
}

DBConnectionPool::~DBConnectionPool()
{
	// 소멸자는 프로그램 종료(static 소멸) 때 불림 -> 락을 잡지 않는 정리 함수 사용
	ReleaseAll();
}

bool DBConnectionPool::Connect(int32 maxConnections, const WCHAR* connString)
{
	WRITE_LOCK;

	// 이미 초기화된 경우
	if (_initialized)
	{
		cout << "DBConnectionPool already initialized!" << endl;
		return false;
	}

	// ODBC 환경 핸들 생성
	if (::SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &_env) != SQL_SUCCESS)
	{
		cout << "SQLAllocHandle ENV FAILED" << endl;
		return false;
	}

	if (::SQLSetEnvAttr(_env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) != SQL_SUCCESS)
	{
		cout << "SQLSetEnvAttr ENV FAILED" << endl;
		ReleaseAll();	// 만든 환경 핸들 정리
		return false;
	}

	_maxConnections = maxConnections;
	// 최소 개수만큼 미리 생성
	_connections.reserve(_maxConnections);

	for (int32 i = 0; i < _maxConnections; i++)
	{
		DBConnection* connection = xnew<DBConnection>();
		if (connection->Connect(_env, connString) == false)
		{
			xdelete(connection);	// 아직 목록에 안 넣은 것 -> 여기서 정리 (전에는 새고 있었음)
			ReleaseAll();			// 앞에서 만든 연결 + 환경 핸들까지 정리 -> 다시 Connect 해도 되는 상태
			return false;
		}
		_connections.push_back(connection);
	}
	_initialized = true;
	cout << "DBConnectionPool Connected. Count : " << maxConnections << endl;
	return true;
}

void DBConnectionPool::Clear()
{
	WRITE_LOCK;
	ReleaseAll();
}

void DBConnectionPool::ReleaseAll()
{
	// 연결들 먼저 (각 DBConnection 소멸자가 접속을 끊고 핸들 해제), 환경 핸들은 마지막
	//   연결이 남아 있는 환경 핸들은 해제가 안 됨
	for (DBConnection* connection : _connections)
	{
		xdelete(connection);
	}
	_connections.clear();

	if (_env != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_ENV, _env);
		_env = SQL_NULL_HANDLE;
	}

	_initialized = false;
}

DBConnection* DBConnectionPool::Pop(uint32 timeoutMs)
{
	const uint64 startTime = ::GetTickCount64();

	while (true)
	{
		// 락을 걸고 꺼낼 수 있는지 확인
		{
			WRITE_LOCK;
			if (_connections.empty() == false)
			{
				DBConnection* connection = _connections.back();
				_connections.pop_back();
				return connection;
			}
		}

		// 타임아웃 시간이 지났으면 포기
		if (::GetTickCount64() > startTime + timeoutMs)
		{
			return nullptr;
		}
			

		// 스레드가 빈 루프를 돌면서 CPU를 잡아먹지 않도록 양보
		this_thread::yield();
	}
}

// 반납
void DBConnectionPool::Push(DBConnection* connection)
{
	WRITE_LOCK;
	_connections.push_back(connection);
}

void DBConnectionPool::HandleKeepAlive()
{
	WRITE_LOCK;

	if (!_initialized) return;

	cout << "[DB] Sending Keep-Alive to " << _connections.size() << " idle connections..." << endl;

	for (DBConnection* connection : _connections)
	{
		// 쿼리를 하나 보내서 MySQL 측의 idle 타이머를 리셋.
		if (connection->Ping() == false)
		{
			cout << "[DB] Keep-Alive Failed for a connection." << endl;
		}

		// 대기 중인 쿼리 결과를 정리 (Unbind)
		connection->Unbind();
	}
}
