-- 기존 종류별 줄(player_item)을 칸으로 옮김: 캐릭터마다 아이템 번호 순서대로 0번 칸부터
--   ROW_NUMBER() OVER (PARTITION BY character_id ...) = 캐릭터마다 1, 2, 3 ... 번호 -> 1을 빼서 칸 번호
--   기존 개수가 새 max_stack(젤리 50)보다 많으면 서버가 불러올 때 max_stack으로 자름 (지금 있는 건 테스트 캐릭터 데이터뿐)
INSERT INTO player_inventory (character_id, slot, item_id, count) SELECT character_id, ROW_NUMBER() OVER (PARTITION BY character_id ORDER BY item_id) - 1, item_id, count FROM player_item;
