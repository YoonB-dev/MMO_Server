#include "pch.h"
#include "Grid.h"
#include "Player.h"
#include "Monster.h"
#include <cmath>
#include <algorithm>

// vector에서 하나 빼기: 찾은 자리에 맨 뒤 원소를 옮기고 맨 뒤를 지움 (순서는 상관없어서 가운데를 당기지 않음)
//   템플릿: PlayerRef 목록과 MonsterRef 목록에 같은 코드를 씀 (컴파일러가 타입마다 하나씩 만들어 줌)
template<typename T>
static void EraseFrom(xvector<T>& list, const T& object)
{
	auto it = std::find(list.begin(), list.end(), object);
	if (it == list.end())
		return;
	*it = std::move(list.back());	// 복사 대신 move: shared_ptr 참조 카운트를 올렸다 내리지 않음
	list.pop_back();
}

Grid::Grid(float worldHalfSize, float cellSize)
	: _worldHalfSize(worldHalfSize), _cellSize(cellSize)
{
	_cellsPerSide = static_cast<int32>(std::ceil(worldHalfSize * 2.f / cellSize));	// 200 / 35 = 5.7 -> 6
	_cells.resize(_cellsPerSide * _cellsPerSide);
}

void Grid::Add(const PlayerRef& player)
{
	player->gridCell = CellIndexOf(*player);
	_cells[player->gridCell].players.push_back(player);
}

void Grid::Add(const MonsterRef& monster)
{
	monster->gridCell = CellIndexOf(*monster);
	_cells[monster->gridCell].monsters.push_back(monster);
}

void Grid::Remove(const PlayerRef& player)
{
	if (player->gridCell < 0)
		return;
	EraseFrom(_cells[player->gridCell].players, player);
	player->gridCell = -1;
}

void Grid::Remove(const MonsterRef& monster)
{
	if (monster->gridCell < 0)
		return;
	EraseFrom(_cells[monster->gridCell].monsters, monster);
	monster->gridCell = -1;
}

void Grid::Update(const PlayerRef& player)
{
	const int32 cell = CellIndexOf(*player);
	if (cell == player->gridCell)
		return;
	EraseFrom(_cells[player->gridCell].players, player);
	_cells[cell].players.push_back(player);
	player->gridCell = cell;
}

void Grid::Update(const MonsterRef& monster)
{
	const int32 cell = CellIndexOf(*monster);
	if (cell == monster->gridCell)
		return;
	EraseFrom(_cells[monster->gridCell].monsters, monster);
	_cells[cell].monsters.push_back(monster);
	monster->gridCell = cell;
}

// -100 ~ 100 -> 0 ~ 5. 월드 밖 좌표도 가장자리 칸으로 (몬스터 배회 지점이 경계를 살짝 넘을 수 있음)
int32 Grid::ToCol(float x) const
{
	const int32 col = static_cast<int32>(std::floor((x + _worldHalfSize) / _cellSize));
	return std::clamp(col, 0, _cellsPerSide - 1);
}

int32 Grid::CellIndexOf(GameObject& object) const
{
	const Protocol::PosInfo* pos = object.GetPosInfo();
	return ToCol(pos->z()) * _cellsPerSide + ToCol(pos->x());	// 행 = z, 열 = x
}

// (x, z)를 중심으로 한 변 2 * range인 정사각형에 걸치는 칸들. 원(거리 range)은 이 정사각형 안에 들어감
Grid::CellRange Grid::GetCellRange(float x, float z, float range) const
{
	return CellRange{ ToCol(x - range), ToCol(x + range), ToCol(z - range), ToCol(z + range) };
}
