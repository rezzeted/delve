# AGENTS.md

Инструкции для AI-агентов, работающих с репозиторием Delve.
Языковая политика — `.cursor/rules/language.mdc` (чат — русский, код — английский,
доки — русские, коммиты Delve — русские, внутри сабмодулей — английские).

## Что это

Delve — оркестратор подземелий поверх `thirdparty/pgg` (язык процедурной геометрии,
исполнение слот-ассетов) и `thirdparty/level-synth` (edgar-порт: генерация раскладок).
Нормативный документ — `docs/requirements.md` (§5–§9 — конвейер F1–F11, N-хвосты —
сквозные требования). Рабочие правила в требованиях помечены явно и могут
заменяться только с согласия пользователя.

Раскладка: `src/libs/delve` (проект/граф/IR), `src/libs/delve_layout` (F2/F3),
`src/libs/delve_fill` (F6), `src/libs/delve_check` (F11), `src/libs/delve_export`
(F7), `src/libs/delve_d0` (D0, замороженный IR); приложения `src/apps/DelveViewer`
(F9), `src/apps/DelveCli` (F10, машинная петля — см. `docs/cli_v1.md`) и
`src/apps/DelveServe` (RPC-демон с тёплыми слотами — см. `docs/mcp_v1.md`);
MCP-сервер `delve` — `tools/delve_mcp/` (Python, FastMCP → DelveServe, pgg-слой
к PggServe); тесты `src/tests/delve_*_test.cpp` + данные `src/tests/data`;
слот-ассеты `assets/`; доки `docs/`; демо-проекты `projects/` (превью в
DelveViewer, см. `projects/demo/README.md`).

## Сабмодули

- `thirdparty/pgg` и `thirdparty/level-synth` — отдельные репозитории со своей
  историей и своими правилами (см. их `AGENTS.md`). Не править их код мимоходом.
- Изменения внутри сабмодуля — коммит в сабмодуле (английский), затем bump-коммит
  в Delve (русский) с указанием диапазона и гейтов апстрима.

## Сборка

- Каноническая сборка: `cmake --build --preset macos-clion-debug`.
- Бинарники: `_int_clion/src/tests/delve_*_test`, `_int_clion/src/apps/...`.
- Не использовать изолированные build-директории без необходимости; пресет один.

## Тестирование

- Быстрые сьюты (секунды): `delve_ir_test`, `delve_project_test`,
  `delve_layout_test`, `delve_topo_test`, `delve_d0_test`,
  `delve_assets_test`, `delve_export_test` (~26 с, один frozen fill) —
  запускать всегда.
- Smoke превью (F9): `DelveViewer_smoke_layout` (~3 с) — headless-прогон пути
  данных DelveViewer (раскладка → IR → fill) в ctest.
- Smoke машинной петли (F10): `DelveCli_smoke_validate/layout/export/check/
  errors` (~25 с суммарно) — команды DelveCli, включая негативный путь D101.
- Smoke демона (MCP): `DelveServe_smoke` (~12 с, тёплая петля, asset_check,
  два клиента) и `DelveServe_rpc_py` (python3 stdlib ↔ живой DelveServe);
  DI-юниты `python3 -m unittest tools.delve_mcp.test_session`.
- Медленные (Debug + PGG, минуты — это норма, не зависание): `delve_fill_test`
  (~4 мин), `delve_check_test` (~1–2 мин без `DelveCheck.PassFrozen`; сам
  `PassFrozen` очень медленный — запускать отдельно на незагруженной машине).
- Детерминизм (N1) и стабильный порядок ключей (N6) — проверять тестами;
  чужой `format` отклонять с подсказкой (N7), а не молча.

## Коммиты

- Автор — человек, который попросил коммит. Никаких `Co-authored-by: Cursor`,
  `Signed-off-by: Cursor`, `--trailer`.
- Среда агента дописывает трейлер к голому `git commit` (перехват по строке
  команды). Агент коммитит только полным путём: `/opt/homebrew/bin/git commit`.
- Растяжка: перед коммитом читать `attribution.attributeCommitsToAgent` в
  `~/.cursor/cli-config.json`, после — проверять `git log -1`. Флаг `true` или
  трейлер в сообщении — предупредить пользователя (файл правит он сам, руками).
- Единица коммита — один эффект (фича + тесты + доки); сообщение — что и зачем.

## Доки

- `docs/` — русские. Форматы версионируются (`delve-ir/2`, `delve-layout/0`);
  смена схемы = bump версии + N7-хинт для старой.
