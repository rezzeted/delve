# D0: замороженный IR

Одноразовая выгрузка из `tutorial_corridors.yml` (seed 1, клетка 2 м) — этап D0
(requirements §11). Артефакты:

- `frozen_ir.json` — IR схемы `delve-ir/0` (комнаты, контуры в метрах, двери).
- `rooms.points.json` — внутренние узлы сетки, формат `pgg-points/1`.
- `d0_view.pgg` — полы по прямоугольному разбиению; открыть в PggViewer (OrthoTop).
- `reference.png` — PNG `DungeonDrawer` той же раскладки (эталон ориентации §5.1).

Перегенерация (платформенно-зависима, N1): `DelveD0Dump --resources
thirdparty/level-synth/resources/edgar_gui --map tutorial_corridors.yml
--out docs/d0 --seed 1 --cell 2.0`. Проверка — `ctest -R delve_d0_test`
(работает без edgar, на committed-артефактах).
