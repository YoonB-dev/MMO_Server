#pragma once

// 월드를 정사각형 칸(셀)으로 나눠서, 칸마다 그 안에 있는 플레이어 / 몬스터 / 바닥 드랍을 들고 있음
//   "(x, z)에서 range 안에 있을 수 있는 객체"를 찾을 때 월드 전체가 아니라 그 범위에 걸친 칸만 봄
//   칸은 후보를 좁히는 용도이고, 정확한 거리 판정은 부르는 쪽(Room)이 함
//   Room의 멤버라서 Room JobQueue 안에서만 쓰임 -> 락 없음
//
//   칸 크기 = 시야에서 나가는 반경(35m)으로 잡으면 range 35m 조회는 항상 3x3칸 이하
//   (월드 200m -> 한 변 6칸, 전체 36칸)
struct GridCell
{
	xvector<PlayerRef>	players;
	xvector<MonsterRef>	monsters;
	xvector<DropRef>	drops;		// 바닥 드랍 (움직이지 않아서 Update 없음)
};

class Grid
{
public:
	Grid(float worldHalfSize, float cellSize);

	// 방에 들어올 때 / 나갈 때 한 번
	void	Add(const PlayerRef& player);
	void	Add(const MonsterRef& monster);
	void	Remove(const PlayerRef& player);
	void	Remove(const MonsterRef& monster);
	void	Add(const DropRef& drop);
	void	Remove(const DropRef& drop);

	// 위치가 바뀐 뒤 호출: 칸이 바뀌었으면 옛 칸에서 빼고 새 칸에 넣음 (대부분은 같은 칸이라 비교만 하고 끝)
	void	Update(const PlayerRef& player);
	void	Update(const MonsterRef& monster);

	// (x, z)에서 range 안일 수 있는 칸들의 플레이어 / 몬스터마다 func(const PlayerRef&) 호출
	//   템플릿 + 람다: 결과를 vector에 모아서 돌려주면 shared_ptr 복사가 매번 생기므로, 찾은 자리에서 바로 func를 부름
	//   func 안에서 Add / Remove / Update를 부르면 안 됨 (돌고 있는 vector가 바뀜)
	template<typename Func>
	void ForEachPlayer(float x, float z, float range, Func&& func)
	{
		CellRange r = GetCellRange(x, z, range);
		for (int32 row = r.minRow; row <= r.maxRow; row++)
			for (int32 col = r.minCol; col <= r.maxCol; col++)
				for (const PlayerRef& player : _cells[row * _cellsPerSide + col].players)
					func(player);
	}

	template<typename Func>
	void ForEachMonster(float x, float z, float range, Func&& func)
	{
		CellRange r = GetCellRange(x, z, range);
		for (int32 row = r.minRow; row <= r.maxRow; row++)
			for (int32 col = r.minCol; col <= r.maxCol; col++)
				for (const MonsterRef& monster : _cells[row * _cellsPerSide + col].monsters)
					func(monster);
	}

	template<typename Func>
	void ForEachDrop(float x, float z, float range, Func&& func)
	{
		CellRange r = GetCellRange(x, z, range);
		for (int32 row = r.minRow; row <= r.maxRow; row++)
			for (int32 col = r.minCol; col <= r.maxCol; col++)
				for (const DropRef& drop : _cells[row * _cellsPerSide + col].drops)
					func(drop);
	}

private:
	// 조회할 칸 범위 (양 끝 포함)
	struct CellRange
	{
		int32	minCol, maxCol;
		int32	minRow, maxRow;
	};

	int32		ToCol(float x) const;	// 좌표 -> 칸 번호 (월드 밖이면 가장자리 칸)
	int32		CellIndexOf(GameObject& object) const;
	CellRange	GetCellRange(float x, float z, float range) const;

private:
	float	_worldHalfSize;
	float	_cellSize;
	int32	_cellsPerSide;			// 한 변의 칸 수
	xvector<GridCell>	_cells;		// _cellsPerSide x _cellsPerSide칸, 한 줄로 펼쳐서 [row * _cellsPerSide + col]
};
