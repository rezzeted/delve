# Delve: машинная петля F10 v1 (DelveCli) и экспорт F7

`src/apps/DelveCli`: CLI для всех шагов конвейера (requirements §7, F10) —
агент проходит «проект → экспорт» без GUI. Экспорт (F7) живёт в
`src/libs/delve_export` и переиспользует писателей PGG (`writeObj`,
`writeObjSplitGroups`, `savePointsGeo`). Тесты: `delve_export_test` (gtest) и
smoke-прогоны `DelveCli_smoke_*` в ctest.

## Команды

```
DelveCli <command> [args] [--json] [--assets <dir>]
  validate <project.json>                                  F1: загрузка и проверка проекта
  layout   <project.json> [--seed N] [--attempts N] [-o layout.json]
                                                           F2/F3: каталог + раскладка edgar
  ir       <project.json> [--layout f | --ir f] [-o ir.json]
                                                           F4/F5: построить или нормализовать IR
  fill     <project.json> [--ir f | --layout f] [--threads N]
                                                           F6: наполнение (только статистика)
  export   <project.json> [--ir f | --layout f] [-o dir] [--name n] [--split-groups]
                                                           F6+F7: артефакты уровня
  check    <project.json> [--ir f | --layout f] [--threads N] [--unit s]
                                                           F6+F11: геометрические проверки
```

- `--unit s` у `check` — юнитовый предчек (F11-fast для арт-итерации): только
  юниты, чей id содержит подстроку `s`, прогоняются через per-unit логику
  elements (смешение `@style` внутри элемента, совпадающие грани); глобальные
  проверки (проходимость, спаны, якоря, …) пропускаются. Ноль совпавших
  юнитов — ошибка (опечатка в фильтре). В `--json` добавляется
  `stats.units` — число совпавших юнитов.

- Без `--ir`/`--layout` IR строится из layout-яруса проекта (delve-project/1).
  `--ir` сниффится по ключу `format`: `delve-ir/0` собирается через проект,
  `delve-ir/2|3` читается напрямую (N7: чужой формат — D102).
- Без `-o` команды `layout`/`ir` печатают артефакт в stdout (статистика — в
  stderr), с `-o` пишут в файл. `-o` у `export` — каталог (создаётся), по
  умолчанию `.`; `--name` — базовое имя артефактов (по умолчанию stem проекта,
  а для голого `project.json` — имя его каталога).
- `--assets` перекрывает поиск библиотеки ассетов (по умолчанию — как во
  вьювере: `./assets` от cwd, вверх от exe, вверх от проекта).
- Кэш юнитов F8 — in-memory, живёт между шагами одной команды (в `export`
  наполнение однократное). Дискового слоя нет (после D4).

## Коды возврата

`0` — ок (`check`: все проверки зелёные), `1` — диагностики с ошибками
(невалидные данные, красная F11), `2` — ошибка командной строки или io
(запись артефактов, `--help` после ошибки разбора).

## Диагностики (F10)

Библиотеки по-прежнему отдают строки; CLI заворачивает ошибку каждого шага в
`delve::Diag` (`src/libs/delve/diag.h`) с кодом класса шага. Текстовый вид — в
стиле PGG: `D301 <место>: <ожидание/факт>` + опциональная строка
`  hint: <исправление>`; печать в stderr. `--json` — один объект в stdout:

```json
{"command": "validate", "ok": false,
 "diagnostics": [{"code": "D101", "message": "...", "hint": "..."}],
 "stats": {"...": "..."}}
```

Классы кодов (перечень F10):

| Класс | Коды | Где возникает |
|---|---|---|
| загрузка проекта | D100 io, D101 ключ/тип/значение, D102 чужой `format` (N7) | `load_project`, чтение `--ir`/`--layout` |
| инвариант | D200 (5.4 при загрузке, 5.2 при построении IR) | проект, IR |
| раскладка | D300 каталог/хендофф delve-layout/0, D301 «не разложилось» | F2/F3 |
| слот | D400 (в message — сквозные коды R-A3: `delve/slot`, E-коды PGG) | F6 |
| прогон PGG | D500 (в message — E-коды PGG) | F6 |
| проверка | D600 (message = `F11/<check>: …`) | F11 |

Детали (место, ожидание/факт, имена комнат, PGG-подсказки) остаются в
`message` — библиотеки их уже формулируют; код даёт машинно-читаемый класс.

## Артефакты экспорта (F7)

В каталоге `-o` с базовым именем `<name>`:

- `<name>.obj` — общий меш уровня (Wavefront OBJ): вершинные цвета из `@Cd`
  (расширение `v x y z r g b`, читают Blender/MeshLab/Houdini), нормали `vn`;
  полигональные грани триангулируются при записи.
- `<name>.anchors.json` — якоря, формат `pgg-points/1` (PGG `geo_file`):
  `positions` + `attrs.kind` (int: 1 = light, 2 = spawn, 3 = poi),
  `attrs.label` (string `"<unit_id>#<kind>"`), у ламп `attrs.dir` (vec3).
  Читается обратно `pgg::loadPointsGeo` и PggTool `--param`.
- `<name>.units.json` — атрибуция юнитов, формат `delve-units/1`
  (стабильный порядок ключей, N6): массив `units` с `{id, slot, mesh_begin,
  mesh_end, anchors_begin, anchors_end}` — спаны точек общего меша и якорей.
  Это машинно-читаемые «группы» F7: сам меш групп не несёт (см. fill_v1).
- `<name>.ir.json` — IR рядом (`delve-ir/3`, `write_ir_v2_json`).
- `--split-groups` — дополнительно один OBJ на юнит
  (`<name>.<unit>.obj`, `writeObjSplitGroups` по выпеченным faces-группам;
  символы id вне `[A-Za-z0-9._-]` заменяются на `_`). Грани, не вошедшие ни в
  один юнит (швы), оказываются в `<name>._nogroup.obj`, когда такие есть.

## Петля агента

```
# полный путь одной командой
DelveCli export projects/demo/project.json -o out --json

# или по шагам с артефактами между ними
DelveCli validate projects/demo/project.json
DelveCli layout   projects/demo/project.json -o out/layout.json
DelveCli ir       projects/demo/project.json --layout out/layout.json -o out/ir.json
DelveCli check    projects/demo/project.json --ir out/ir.json
DelveCli export   projects/demo/project.json --ir out/ir.json -o out
```

Каждая ошибка — код класса + место + исправление; правка проекта и повторный
запуск замыкают петлю без человека. Проверка «OBJ открывается во внешнем
просмотрщике» (F7) — визуальная, на артефактах `out/`.
