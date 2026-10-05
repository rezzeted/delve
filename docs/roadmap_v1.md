# Delve: roadmap улучшений конвейера (v1)

Шесть пунктов из разбора болей генерации (октябрь 2026, после решётки
`decor:drain`). Пакеты независимы; внутри пакета — порядок выполнения.
Каждый пункт приземлён на файлы и гейты.

## Пакет A — быстрый фидбек арт-итерации

Боль: геометрическая ошибка ассета сейчас стоит 3 минуты полного F11
(`DelveCli check iso`) или таймаут MCP; контрактные ошибки выхода (D500)
всплывают только на полном fill.

### A1. Юнитовый предчек геометрии (F11-fast) — сделано

- Проблема: F11/elements — проверка строго внутри юнита, но прогоняется на
  всём уровне. Для итерации над одним ассетом нужен один юнит.
- Решение (в коде): `delve::check_units(fill, substr, …)` — per-unit логика
  elements по юнитам с подстрокой в id (ноль совпадений — ошибка);
  `DelveCli check --unit s`, `unit` в RPC `check` и MCP `delve_check`
  (тёплый слот DelveServe — fill уже в памяти). Глобальные проверки
  (спаны, проходимость, якоря) в режиме `--unit` пропускаются.
- Файлы: `src/libs/delve_check/check.{h,cpp}`, `src/apps/DelveCli`,
  `src/apps/DelveServe`, `tools/delve_mcp`.
- Гейты: `delve_check_test` (CheckUnitsFiltersSpans), `DelveCli_smoke_check`,
  живой прогон MCP по юниту решётки за секунды.

### A2. Contract lint в `asset_check` — сделано

- Проблема: забытые группы/`uv`/`vec2`/`vec4` или отсутствие `@style`/`@Cd`
  на выходе ловятся только на сборке fill (D500).
- Решение (в коде): `delve::lint_asset` — отдельный вход рядом со статическим
  `check_asset` (fill_level его НЕ зовёт: fill и так прогоняет ассеты
  по-настоящему). Прогон с минимальными синтетическими входами по контракту
  слота и строгая проверка схемы выхода: нет групп/detail/instances, нет
  vec2/vec4 колонок, `@style: int` + `@Cd: vec3` на точках меша (кроме
  `door`), `@kind: int` на непустых якорях. Падение синтетического прогона
  (богатые входы) — warning «inconclusive», не ошибка. RPC `asset_check`
  зовёт lint при чистой статике; коды диагностик — `delve/lint`.
- Файлы: `src/libs/delve_fill/fill_ra3.cpp` (+ тест ContractLintA2 в
  `delve_assets_test`), `src/apps/DelveServe`.
- Гейты: `delve_assets_test`, живой `delve_asset_check` на грязном ассете
  (черновик в tmp/ с забытой группой даёт delve/lint-диагностику).

## Пакет B — скорость больших проверок

### B3. Инкрементальный F11 поверх кэша F8 — сделано

- Проблема: A1 решает итерацию по одному юниту, но полный check перед
  коммитом всё ещё ~3 минуты, хотя fill закэширован.
- Решение (в коде): вердикты elements кэшируются в записи `UnitCache`
  (`Entry::check`, версия `kElementsCheckVersion` — bump при смене правил);
  спаны `FillResult` несут `cacheKey` (F8-ключ юнита, когда fill шёл с
  кэшем). `check_level_cached` реплеит вердикты reused-юнитов и
  пересчитывает только reran; глобальные проверки дешевы и идут всегда.
  Сообщения хранятся без id юнита и перепрефиксируются на реплее.
  DelveServe `check` зовёт cached-вариант; CLI (one-shot, без кэша) —
  прежний `check_level`.
- Файлы: `src/libs/delve_check`, `UnitCache` (расширение записи),
  `FillResult::UnitSpan.cacheKey`.
- Замер на тёплой петле iso: второй check — секунды вместо ~2–3 минут
  (см. коммит).

## Пакет C — richer decor

### C4. Декор-правила v2 — сделано

- Проблема: `fill.decor` v1 — один предмет на комнату, место случайное,
  без учёта занятого объёма (бочки в углах hall/crypt могут пересечься с
  предметом) и без настенных/потолочных правил.
- Решение (в коде): поля `count` (≥ 1), `min_dist` (≥ 0), `align`
  (`any`/`center`/`near_door`), `radius` (футпринт предмета), `place:
  floor|wall`; дефолты = поведение v1 (iso/demo не менялись). Реестр занятых
  объёмов: `room_fill` эмитит blocker-якоря (`@kind=4`, `@range` = радиус;
  fill_v2 — по одному на бочку), сборка снимает их в `FillResult::occupied`
  (в уровневые якоря не попадают — F11/экспорт неизменны). Заливка
  двухфазная: room_fill прогоняется первым, напольный декор держит зазор
  `o.r + radius + min_dist` от каждого цилиндра и регистрирует свои
  размещения обратно (предметы одного правила не наезжают). `place: wall` —
  кандидаты обходом фасадов как у бра, выбор без возврата.
- Файлы: `project.{h,cpp}`, `fill.{h,cpp}` (splitBlockers, двухфазность,
  expandDecorFloor v2 / expandDecorWall), `assets/codes.pgg` (AK_BLOCKER=4),
  `assets/rooms/fill_v2.pgg` (blocker-якоря), тесты
  (`OccupiedRegistryClearsDecor`, `DecorAlignCenter/NearDoor`,
  `DecorWallPlace`, обновления parity/RA4), `docs/project_v0.md`,
  `docs/fill_v1.md`, `docs/slots_v1.md`, `docs/assets_v1.md`.
- Гейты: быстрые сьюты + smoke зелёные; `OccupiedRegistryClearsDecor` —
  fill_v2 на frozen-уровне, drains не ближе порога к бочкам.
- Живой замер iso: fill 32.7 с (без регресса к 32.4 с до C4),
  `occupied=12` (блокеры бочек hall/crypt/vault + поставленные drains);
  тёплый refill 1.0 с — реестр восстанавливается из F8-попаданий
  идентично.

## Пакет D — честные вырезы

### D5. Вырезы в полу под напольный декор — сделано

- Проблема: решётка дренажа «врезана в пол» пересечением с room_fill, а
  «шахта» — чёрный диск-обманка поверх камней. Пол о дыре не знает.
- Решение (в коде): у decor-правила поле `cut_r` (≥ 0, дефолт 0); у слота
  `room_fill` опциональный вход `cuts` (geo<points>, `@range` = радиус,
  объявляется без дефолта — R-A2 для geo-экстра смягчён, хост биндит
  всегда); у слота `decor` опциональный `pit: int = 0`. Заливка
  трёхфазная: room_fill → развёртка декора (предметы с `cut_r` в комнатах
  с `cuts` откладываются) → повторный прогон затронутых room_fill с
  реальными точками + постановка отложенного декора с `pit = 1`. Блокеры
  и `occupied` повторный прогон не дублирует; ключи F8 различаются
  набором биндингов — оба прогона тёплые. fill_v2 строит ствол
  (`arc_shell`-облицовка, тёмное дно, подложка-кольцо), фартук радиальных
  блоков и выбивает камни/постель (потолок не режется); drain_v2 при
  `pit = 1` убирает обманки (оконтовка, диск «воды»).
- Файлы: `project.{h,cpp}` (`cut_r`), `fill.h`/`fill_ra3.cpp` (declared
  params, R-A2 geo-экстра), `fill.cpp` (трёхфазная заливка),
  `assets/rooms/fill_v2.pgg` (+фикстуры cuts), `assets/decor/drain_v2.pgg`,
  `projects/iso/project.json` (drain: `cut_r 0.28`), тесты
  (`DecorFloorCuts`, `runDrain(pit)`, обновления RA4), `docs/project_v0.md`,
  `docs/fill_v1.md`, `docs/slots_v1.md`, `docs/assets_v1.md`.
- Гейты: быстрые сьюты + smoke + check (без PassFrozen) зелёные;
  `DecorFloorCuts` — колодец реальный (пол в центре пуст, дно и фартук
  есть, блокеры не дублируются).
- Живой замер iso: fill 37.8 с (9 комнат, `occupied=12`), тёплый refill
  1.2 с, reran=0 — оба ключа F8 (пустой и с cuts) тёплые.

## Пакет E — pgg UX (сабмодуль pgg, коммиты на английском)

### E6. Диагностика pgg: did-you-mean + пачка ошибок — сделано

- Проблема: чекер fail-fast (одна ошибка за прогон); E201/E604 без
  кандидатов; биндинги `pgg_params` слетают при перечитывании файла по mtime;
  `pgg_render` без `file=` берёт последний `pgg_load`, а относительный `out=`
  резолвится от корня pgg-репо.
- Решение (в коде): общий хелпер `eval/suggest.{h,cpp}` в pgg (Levenshtein с
  отсечением + правило префикса, детерминированный порядок); did-you-mean в
  E201 (кандидаты — builtin'ы), обоих E505 (def'ы модуля / неймспейсы), E103
  validate+typecheck (видимые имена); `suggestBuiltinNames` (промах `docs`)
  переведена на тот же хелпер. По аудиту «fail-fast» оказался межфазными
  gate'ами (parse → imports → expand → typecheck), внутри фазы все ошибки и
  так собираются пачкой — gate семантически необходим, не трогаем. PggServe:
  биндинги param переживают reload (явный load и F4) — восстанавливаются по
  имени; `params` отвечает `suggestions` для неизвестных имён. delve-MCP:
  `pgg_render out=` резолвится от корня delve-репо (`file=` без аргумента уже
  подставлялся клиентом — `PggSession._with_file`, тест был).
- Файлы: pgg `5bd199c` (suggest.{h,cpp}, typecheck/expand/validate,
  builtin_docs, DocumentSession/ServeRuntime, тесты suggest/typecheck/
  validate/module, implementation.md, serve_rpc.md); delve — bump +
  `tools/delve_mcp/pgg_layer.py` (`_absolutize_out`), тест
  `test_render_out_absolutized_against_delve_root`, `docs/mcp_v1.md`.
- Гейты: pgg_tests без новых падений (7 золотых mismatch'ей — дрейф платформы,
  воспроизводятся на чистом HEAD); `PggServe --smoke` PASS; живой RPC —
  биндинги пережили auto-reload, `suggestions` работает; delve быстрые сьюты
  зелёные, python unittest 68 OK.
