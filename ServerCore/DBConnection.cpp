#include "pch.h"
#include "DBConnection.h"

// 와이드 문자열(UTF-16) -> UTF-8
//   콘솔은 main에서 UTF-8로 맞춰 두었으므로(SetConsoleOutputCP) UTF-8로 바꿔서 cout으로 출력하면 한글도 그대로 보임
//   (ConfigManager.cpp의 WideToMultiByte도 같은 일을 하지만 헤더에 선언이 없어서, 이 파일 전용으로 둠)
static std::string ToUtf8(const WCHAR* str)
{
	const int32 size = ::WideCharToMultiByte(CP_UTF8, 0, str, -1, nullptr, 0, nullptr, nullptr);	// 끝의 '\0' 포함 크기
	if (size <= 1)
		return "";

	std::string result(size, '\0');
	::WideCharToMultiByte(CP_UTF8, 0, str, -1, &result[0], size, nullptr, nullptr);
	result.resize(size - 1);	// 끝의 '\0'은 std::string이 따로 관리하므로 내용에서 뺌
	return result;
}

DBConnection::~DBConnection()
{
	Clear();
}

bool DBConnection::Connect(SQLHENV henv, const WCHAR* connString)
{
	if (::SQLAllocHandle(SQL_HANDLE_DBC, henv, &_connection) != SQL_SUCCESS)
	{
		return false;
	}

	// SQLDriverConnectW가 const가 아닌 포인터를 받아서 복사본을 넘김
	//   (전에는 WCHAR[MAX_PATH] 고정 버퍼라 260자를 넘으면 wcscpy_s가 프로그램을 바로 종료시켰음)
	//   ServerCore는 C++14라 data()가 const 포인터 -> &connStr[0] (문자열은 메모리에 연속으로 있음)
	std::wstring connStr = connString;

	WCHAR resultString[MAX_PATH] = { 0 };
	SQLSMALLINT resultStringLen = 0;

	SQLRETURN ret = ::SQLDriverConnectW(
		_connection,
		NULL,
		reinterpret_cast<SQLWCHAR*>(&connStr[0]),
		SQL_NTS,
		OUT reinterpret_cast<SQLWCHAR*>(resultString),
		_countof(resultString),
		OUT &resultStringLen,
		SQL_DRIVER_NOPROMPT
	);

	// 접속 결과를 먼저 확인: 실패한 연결로 STMT를 만들면 그것도 실패해서 진짜 이유를 못 찍고 끝남
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		HandleError(ret, SQL_HANDLE_DBC, _connection);	// 접속 에러는 연결 핸들(DBC)에 기록됨
		return false;
	}

	if (::SQLAllocHandle(SQL_HANDLE_STMT, _connection, &_statement) != SQL_SUCCESS)
	{
		HandleError(SQL_ERROR, SQL_HANDLE_DBC, _connection);
		return false;
	}

	return true;
}

// 만든 반대 순서로 정리: STMT -> 접속 끊기 -> DBC
//   접속 중인 DBC는 SQLDisconnect 없이는 해제가 안 됨 (에러만 나고 그대로 남음)
void DBConnection::Clear()
{
	if (_statement != SQL_NULL_HANDLE)
	{
		::SQLFreeHandle(SQL_HANDLE_STMT, _statement);
		_statement = SQL_NULL_HANDLE;
	}

	if (_connection != SQL_NULL_HANDLE)
	{
		::SQLDisconnect(_connection);	// 접속 안 된 핸들이면 에러만 돌려주고 아무 일도 안 함
		::SQLFreeHandle(SQL_HANDLE_DBC, _connection);
		_connection = SQL_NULL_HANDLE;
	}
}

bool DBConnection::Execute(const WCHAR* query)
{
	SQLRETURN ret = ::SQLExecDirectW(_statement, (SQLWCHAR*)query, SQL_NTSL);
	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
	{
		return true;
	}

	HandleError(ret);
	return false;
}

bool DBConnection::Fetch()
{
	SQLRETURN ret = ::SQLFetch(_statement);
	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
	{
		return true;
	}

	if (ret == SQL_NO_DATA)
	{
		return false;
	}

	if (ret == SQL_ERROR)
	{
		HandleError(ret);
		return false;
	}

	return true;
}

int32 DBConnection::GetRowCount()
{
	SQLLEN count = 0;
	SQLRETURN ret = ::SQLRowCount(_statement, OUT & count);

	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
	{
		return static_cast<int32>(count);
	}

	return -1;
}

void DBConnection::Unbind()
{
	if (_statement == SQL_NULL_HANDLE) return;

	::SQLFreeStmt(_statement, SQL_UNBIND);
	::SQLFreeStmt(_statement, SQL_RESET_PARAMS);
	::SQLFreeStmt(_statement, SQL_CLOSE);
}

bool DBConnection::Ping()
{
	return Execute(L"SELECT 1;");
}

bool DBConnection::BindParam(int32 paramIndex, bool* value, SQLLEN* index)
{
	return BindParam(paramIndex, SQL_C_TINYINT, SQL_TINYINT, size32(bool), value, index);
}

bool DBConnection::BindParam(int32 paramIndex, float* value, SQLLEN* index)
{
	return BindParam(paramIndex, SQL_C_FLOAT, SQL_REAL, 0, value, index);
}

bool DBConnection::BindParam(int32 paramIndex, double* value, SQLLEN* index)
{
	return BindParam(paramIndex, SQL_C_DOUBLE, SQL_DOUBLE, 0, value, index);
}

bool DBConnection::BindParam(int32 paramIndex, int8* value, SQLLEN* index)
{
	return BindParam(paramIndex, SQL_C_TINYINT, SQL_TINYINT, size32(int8), value, index);
}

bool DBConnection::BindParam(int32 paramIndex, int16* value, SQLLEN* index)
{
	return BindParam(paramIndex, SQL_C_SHORT, SQL_SMALLINT, size32(int16), value, index);
}

bool DBConnection::BindParam(int32 paramIndex, int32* value, SQLLEN* index)
{
	return BindParam(paramIndex, SQL_C_LONG, SQL_INTEGER, size32(int32), value, index);
}

bool DBConnection::BindParam(int32 paramIndex, int64* value, SQLLEN* index)
{
	return BindParam(paramIndex, SQL_C_SBIGINT, SQL_BIGINT, size32(int64), value, index);
}

bool DBConnection::BindParam(int32 paramIndex, TIMESTAMP_STRUCT* value, SQLLEN* index)
{
	return BindParam(paramIndex, SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, size32(TIMESTAMP_STRUCT), value, index);
}

bool DBConnection::BindParam(int32 paramIndex, const WCHAR* str, SQLLEN* index)
{
	SQLULEN size = static_cast<SQLULEN>((::wcslen(str) + 1) * sizeof(WCHAR));
	*index = SQL_NTSL;

	if (size > WVARCHAR_SIZE)
	{
		return BindParam(paramIndex, SQL_C_WCHAR, SQL_WLONGVARCHAR, size, (SQLPOINTER)str, index);
	}
	else
	{
		return BindParam(paramIndex, SQL_C_WCHAR, SQL_WVARCHAR, size, (SQLPOINTER)str, index);
	}
}

bool DBConnection::BindParam(int32 paramIndex, const BYTE* bin, int32 size, SQLLEN* index)
{
	if (bin == nullptr)
	{
		*index = SQL_NULL_DATA;
		size = 1;
	}
	else
	{
		*index = size;
	}

	if (size > BINARY_MAX)
	{
		return BindParam(paramIndex, SQL_C_BINARY, SQL_LONGVARBINARY, size, (BYTE*)bin, index);
	}
	else
	{
		return BindParam(paramIndex, SQL_C_BINARY, SQL_BINARY, size, (BYTE*)bin, index);
	}
}

bool DBConnection::BindCol(int32 columnIndex, bool* value, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_TINYINT, size32(bool), value, index);
}

bool DBConnection::BindCol(int32 columnIndex, float* value, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_FLOAT, size32(float), value, index);
}

bool DBConnection::BindCol(int32 columnIndex, double* value, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_DOUBLE, size32(double), value, index);
}

bool DBConnection::BindCol(int32 columnIndex, int8* value, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_TINYINT, size32(int8), value, index);
}

bool DBConnection::BindCol(int32 columnIndex, int16* value, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_SHORT, size32(int16), value, index);
}

bool DBConnection::BindCol(int32 columnIndex, int32* value, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_LONG, size32(int32), value, index);
}

bool DBConnection::BindCol(int32 columnIndex, int64* value, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_SBIGINT, size32(int64), value, index);
}

bool DBConnection::BindCol(int32 columnIndex, TIMESTAMP_STRUCT* value, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_TIMESTAMP, size32(TIMESTAMP_STRUCT), value, index);
}

bool DBConnection::BindCol(int32 columnIndex, WCHAR* str, int32 size, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_C_WCHAR, size32(WCHAR) * size, str, index);
}

bool DBConnection::BindCol(int32 columnIndex, BYTE* bin, int32 size, SQLLEN* index)
{
	return BindCol(columnIndex, SQL_BINARY, size, bin, index);
}

bool DBConnection::BindParam(SQLUSMALLINT paramIndex, SQLSMALLINT cType, SQLSMALLINT sqlType, SQLULEN len, SQLPOINTER ptr, SQLLEN* index)
{
	 SQLRETURN ret = ::SQLBindParameter(_statement, paramIndex, SQL_PARAM_INPUT, cType, sqlType, len, 0, ptr, 0, index);
	 if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	 {
		 HandleError(ret);
		 return false;
	 }

	 return true;
}

bool DBConnection::BindCol(SQLUSMALLINT columnIndex, SQLSMALLINT cType, SQLLEN len, SQLPOINTER value, SQLLEN* index)
{
	SQLRETURN ret = ::SQLBindCol(_statement, columnIndex, cType, value, len, index);
	if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
	{
		HandleError(ret);
		return false;
	}

	return true;
}

// 기존 호출(HandleError(ret))은 그대로: 쿼리 에러 -> 문장 핸들(STMT)에서 꺼냄
void DBConnection::HandleError(SQLRETURN ret)
{
	HandleError(ret, SQL_HANDLE_STMT, _statement);
}

// 에러가 기록된 핸들을 직접 지정 (접속 에러는 DBC)
void DBConnection::HandleError(SQLRETURN ret, SQLSMALLINT handleType, SQLHANDLE handle)
{
	if (ret == SQL_SUCCESS || handle == SQL_NULL_HANDLE)
	{
		return;
	}

	SQLSMALLINT index = 1;
	SQLWCHAR sqlState[MAX_PATH] = { 0 };
	SQLINTEGER nativeError = 0;
	SQLWCHAR errMsg[SQL_MAX_MESSAGE_LENGTH] = { 0 };	// 512 (전에는 MAX_PATH 260): 긴 SQL 문법 에러가 잘리지 않게
	SQLSMALLINT errMsgLen = 0;
	SQLRETURN errRet = 0;

	while (true)
	{
		errRet = ::SQLGetDiagRecW(
			handleType,
			handle,
			index,
			OUT sqlState,
			OUT & nativeError,
			OUT errMsg,
			_countof(errMsg),
			OUT & errMsgLen
		);

		if (errRet == SQL_NO_DATA)
		{
			break;
		}

		if (errRet != SQL_SUCCESS && errRet != SQL_SUCCESS_WITH_INFO)
		{
			break;
		}

		// TODO: Log Error
		// wcout.imbue(std::locale("kor"))는 쓰지 않음: wcout 전체의 숫자 형식까지 바꾸고(1,049) 계속 남으며,
		//   CP949로 내보내서 UTF-8 콘솔에서 한글이 깨짐 -> 메시지를 직접 UTF-8로 바꿔서 cout
		cout << "[ODBC Error] SQLState: " << ToUtf8(sqlState) << ", NativeError: " << nativeError
			<< ", Message: " << ToUtf8(errMsg) << endl;
		index++;
	}
}
