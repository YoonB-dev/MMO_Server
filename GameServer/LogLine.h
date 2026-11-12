#pragma once
#include <sstream>

// 서버 콘솔 로그 한 줄. 여러 스레드(IOCP 워커, Room 줄, DB 줄)가 동시에 찍어도 줄이 섞이지 않게 함
//   사용: LogLine() << "[DB] loaded character: account " << accountId;   (endl은 붙이지 않음)
//
//   cout << a << b << endl은 <<마다 따로 출력돼서, 그 사이에 다른 스레드의 출력이 끼어들 수 있음
//   -> 한 줄을 _line에 다 모은 뒤, 소멸자에서 락을 잡고 한 번에 출력
//   LogLine()은 이름 없는 임시 객체라 그 문장의 ; 에서 소멸함 = 줄을 다 모은 직후
//   (ServerCore가 직접 찍는 로그는 이 락을 안 써서 섞일 수 있음)
class LogLine
{
public:
	~LogLine()
	{
		LockGuard guard(s_lock);
		cout << _line.str() << endl;
	}

	// 무엇이든(문자열, 숫자, float ...) ostringstream에 넘김. 자기 참조를 돌려줘서 << 를 이어 붙일 수 있음
	template<typename T>
	LogLine& operator<<(const T& value)
	{
		_line << value;
		return *this;
	}

private:
	ostringstream		_line;
	inline static Mutex	s_lock;		// C++17: 클래스 안에 정의해도 프로그램 전체에 하나 (모든 LogLine이 같은 락)
};
