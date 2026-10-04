"""Delve MCP server — agent tooling over the DelveServe RPC (+ a pgg layer).

Stdio transport. Started as the single MCP server ``delve`` via
``python3 -m tools.delve_mcp.launch`` (venv) then ``python -m tools.delve_mcp``.

Thin proxy to the DelveServe RPC (TCP + line-delimited JSON on
127.0.0.1:9879, env ``DELVE_SERVE_PORT``; see ``src/apps/DelveServe``). The
same Python session auto-starts ``DelveServe`` on every OS; if the binary is
missing the tools return ``kind=need_build`` with configure/build argv. Every
response keeps the RPC envelope (``{"ok": true, "data": ...}`` /
``{"ok": false, "error"}``). Slot identity is the canonical project path.

The ``pgg_*`` tools are a lazy second layer onto PggServe (127.0.0.1:9878,
env ``PGG_SERVE_PORT``; repo ``thirdparty/pgg``) for slot-asset debugging —
probe/render of a single .pgg without a full level fill. PggServe starts only
on the first ``pgg_*`` call; its ``lib_roots`` default is derived from the
delve context (see ``pgg_layer.py``).
"""

from __future__ import annotations

from typing import Any, Optional

from mcp.server.fastmcp import FastMCP

from tools.delve_mcp.pgg_layer import PggLayer
from tools.delve_mcp.session import DelveSession

_INSTRUCTIONS = """Delve MCP — агентская петля «проект → наполнение → проверки → экспорт»
через демона DelveServe (тёплые слоты проектов, F8-кэш юнитов между вызовами).

Типовой цикл:

1. ``delve_status`` — демон жив (поднимается автоматически при первом вызове).
   Если ``error.kind=need_build`` — собрать DelveServe командами из
   ``error.build`` (по порядку, ``cwd`` = корень репо) и повторить вызов;
   MCP сам стартует бинарь.
2. ``delve_load(path)`` — слот проекта + статическая проверка за миллисекунды:
   ``has_errors=false`` → проект валиден. Слот = канонический путь проекта
   (LRU, максимум 4). После load аргумент ``file`` в остальных инструментах
   можно не передавать — подставляется текущий файл этого MCP-клиента.
3. ``delve_fill`` — первый вызов холодный: раскладка с дефолтами (auto-layout)
   + полный fill. Дальнейшие вызовы тёплые: демон сам перечитывает проект по
   mtime и пересчитывает только юниты с изменённым входом (F8; смотри
   ``reused``/``reran`` в ответе).
4. Правка fill-яруса проекта на диске (параметры наполнения, слоты, правила)
   → сразу ``delve_fill`` — явный load не нужен.
5. Правка graph/rooms/layout-яруса (комнаты, проходы, шаблоны, seed) → явный
   ``delve_layout`` (auto-reload раскладку НЕ перегенерирует), затем
   ``delve_fill`` — неизменённые юниты переиспользуются из того же кэша.
6. ``delve_check`` — fill + проверки F11 (диагностики D600).
7. ``delve_export(out=...)`` — экспорт OBJ после fill.

Отладка и инспекция: ``delve_validate`` — статическая проверка файла с диска
без прогона; ``delve_asset_check(slot, asset)`` — статическая проверка
слот-ассета (.pgg) в контексте проекта, без полного fill;
``delve_units``/``delve_provenance(room)`` — юниты и цепочки происхождения
последнего fill; ``delve_ir`` — текст delve-ir/3.

Отладка слот-ассета «под микроскопом» (pgg-слой, демон PggServe — поднимается
лениво на первом pgg_* вызове; его нет → ``need_build`` с командами сборки
pgg, ``cwd`` = thirdparty/pgg). Когда ``delve_asset_check`` красный или нужны
числа/картинка одного .pgg без полного fill уровня:

1. ``pgg_load(path)`` — path ассета относительно корня delve
   (``assets/walls/facing_v1.pgg``) или абсолютный; ``lib_roots`` подставится
   сам (каталог ассета + assets delve + корни проекта).
2. ``pgg_params({...})`` — если у ассета обязательные входы слота (load
   ответил E604: seg/cuts/zones у delve-ассетов): значение ``"@<файл>"``
   грузит фикстуру pgg-points/1 рядом с .pgg (``@facing_v1.seg.points.json``
   и т.п.).
3. ``pgg_probe(spec="…")`` — числа: bbox, stats, schema узла (инспекторы как в
   pgg: schema/stats/bbox[group=…]/check/hist[…]).
4. ``pgg_render(node=…)`` — кадр узла (нет GPU/окна у демона — ответ
   ``no_gpu``: тогда обходиться probe + asset_check).
5. Правка .pgg на диске → сразу повторный ``pgg_probe``/``pgg_render``: у
   PggServe авто-reload по mtime (F4, в ответе reloaded=true), явный
   ``pgg_load`` после правки не нужен.
Перед grep по спеке языка — ``pgg_docs("<builtin>")``.

Конверты: ``{"ok":true,"data":...}`` или ``{"ok":false,"error":{"kind",...}}``.
kind — ``bad_args``/``no_file``/``no_layout``/``no_fill``/``not_found``/
``busy``/``io_error``/``need_build``/``unreachable`` или D-код шага конвейера
(D100/D101/D102/D200/D300/D301/D500). ``restarted=true`` в ответе — демон был
перезапущен после пересборки бинаря, слоты перезагружены (``restart``).
``stale_binary`` — на порту живёт чужой демон старше текущей сборки: остановите
его, MCP поднимет новый бинарь сам.
"""

mcp = FastMCP("delve", instructions=_INSTRUCTIONS)

_session = DelveSession()

# Lazy pgg layer: the PggSession (and PggServe itself) appears only on the
# first pgg_* call — delve tools never touch it.
_pgg_layer = PggLayer(_session)


def _call(op: str, args: Optional[dict[str, Any]] = None) -> dict:
    return _session.call(op, args)


def _with_file(args: dict[str, Any], file: Optional[str]) -> dict[str, Any]:
    if file:
        args = dict(args)
        args["file"] = file
    return args


@mcp.tool()
def delve_status() -> dict:
    """Живость DelveServe и слотов проектов.

    При живом RPC — data: {serve:"running", binary?, rpc:{host,port},
    slots:[{file,has_layout,has_ir,has_fill}], uptime_s, port, assets_dir,
    workers}. Если бинаря нет — ok=false, error.kind=need_build (error.build —
    готовые шаги configure/build с argv и cwd, expected, hint).
    Пример: delve_status().
    """
    return _session.status()


@mcp.tool()
def delve_load(path: str) -> dict:
    """Загрузить delve-проект в слот + статическая проверка (без прогона).

    path — путь до project.json (относительно cwd демона = корня репо, или
    абсолютный). Ответ почти всегда ok=true: {file (канонический),
    diagnostics:[{code,message,hint?,warning?}], has_errors,
    session:{file}?}. has_errors=true (D100/D101/D102/D200) — проект битый,
    слот НЕ создан (session отсутствует). После успешного load файл становится
    текущим для этого MCP-клиента: ``file`` в остальных инструментах можно
    опускать. Явный load после правок на диске не нужен — fill/check/export
    перечитывают проект по mtime сами.
    Пример: delve_load("projects/demo/project.json") → has_errors=false.
    """
    return _call("load", {"path": path})


@mcp.tool()
def delve_validate(file: Optional[str] = None) -> dict:
    """Статическая проверка проекта, перечитанного с диска (слот не трогается).

    file — слот (канонический путь); без него — текущий файл клиента (после
    delve_load). Ответ data: {diagnostics:[{code,message,hint?,warning?}],
    has_errors, session:{file}}. Миллисекунды — дешёвый способ поймать
    D1xx/D200 до дорогого fill.
    Пример: delve_validate() после правки project.json.
    """
    return _call("validate", _with_file({}, file))


@mcp.tool()
def delve_layout(file: Optional[str] = None, seed: Optional[int] = None,
                 attempts: Optional[int] = None) -> dict:
    """Явная перераскладка проекта (F2/F3) + сброс производных IR/fill.

    Нужна после правки layout-яруса (rooms/passages/templates/corridors) или
    graph-яруса проекта: auto-reload внутри fill раскладку НЕ перегенерирует.
    seed — переопределение seed проекта на один прогон (слот не меняется);
    attempts — число попыток генератора (по умолчанию 4). file — слот.
    Ответ data: {seed_used, attempt_used, ms, session:{file}}.
    Ошибки: no_layout — у delve-project/0 нет layout-яруса; D300/D301 —
    каталог/генерация раскладки.
    Пример: delve_layout(seed=42) → seed_used=42.
    """
    return _call("layout", _with_file({"seed": seed, "attempts": attempts}, file))


@mcp.tool()
def delve_ir(file: Optional[str] = None, out: Optional[str] = None) -> dict:
    """IR уровня (delve-ir/3) из текущей раскладки слота.

    Раскладка при необходимости строится автоматически (auto-layout с
    дефолтами, как у fill). out — куда записать JSON (иначе текст возвращается
    в ответе). file — слот. Ответ data: {format:"delve-ir/3", text} или
    {format:"delve-ir/3", wrote:out, session:{file}}.
    Ошибки: no_layout, D200 (сборка IR), D500 (сериализация), io_error.
    Пример: delve_ir(out="tmp/level.ir.json").
    """
    return _call("ir", _with_file({"out": out}, file))


@mcp.tool()
def delve_fill(file: Optional[str] = None, threads: Optional[int] = None) -> dict:
    """Наполнение уровня (F6) — тёплый инкрементальный refill слота.

    Первый вызов после load холодный: auto-layout (если не было delve_layout)
    + полный fill. Правки fill-яруса проекта на диске подхватываются
    автоматически (перечитывание по mtime), F8-кэш пересчитывает только
    юниты с изменённым входом. Правка layout-яруса требует явного
    delve_layout. threads — потоки PGG (0 = авто). file — слот.
    Ответ data: {rooms, bodies, facings, nodes, doors, lamps, occupied, reused,
    reran, ms, session:{file}} — reused+reran доказывают тёплую петлю;
    occupied — число занятых цилиндров реестра C4 (блокеры room_fill +
    поставленный напольный декор).
    Ошибки: D100 (нет assets), D4xx/D5xx — коды fill в error.kind.
    Пример: delve_fill() → {"rooms": 3, "reused": 0, "reran": 12, ...}.
    """
    return _call("fill", _with_file({"threads": threads}, file))


@mcp.tool()
def delve_check(file: Optional[str] = None, unit: Optional[str] = None) -> dict:
    """Fill + проверки F11 (геометрия, проходимость, лампы).

    file — слот. unit — подстрока id юнита: проверяется только геометрия
    юнитов с такой подстрокой (быстрый предчек арт-итерации, глобальные
    проверки пропускаются; ноль совпадений — ошибка). Ответ data: {errors,
    diagnostics:[{code:"D600",message,...}], has_errors, units? (число
    совпавших юнитов в режиме unit), session:{file}}. has_errors=false —
    уровень проходит проверки.
    Пример: delve_check() после delve_fill; delve_check(unit="drain").
    """
    return _call("check", _with_file({"unit": unit}, file))


@mcp.tool()
def delve_export(out: str, file: Optional[str] = None, name: Optional[str] = None,
                 split_groups: bool = False) -> dict:
    """Экспорт уровня в OBJ (F7) после fill.

    out — каталог назначения (создаётся); name — базовое имя файлов (по
    умолчанию stem проекта; для project.json — имя его каталога);
    split_groups=true — отдельные OBJ на группу. file — слот.
    Ответ data: {written:[...пути...], session:{file}}.
    Ошибки: bad_args (нет out), io_error.
    Пример: delve_export(out="tmp/export_demo").
    """
    return _call("export", _with_file({"out": out, "name": name,
                                       "split_groups": split_groups}, file))


@mcp.tool()
def delve_units(file: Optional[str] = None) -> dict:
    """Юниты последнего fill слота (спаны мешей/якорей на юнит).

    file — слот. Ответ data: {units:[{id, slot, mesh:[begin,end],
    anchors:[begin,end]}], session:{file}}.
    Ошибка no_fill — fill ещё не было для этого слота.
    Пример: delve_units() после delve_fill.
    """
    return _call("units", _with_file({}, file))


@mcp.tool()
def delve_provenance(room: str, file: Optional[str] = None,
                     key: Optional[str] = None) -> dict:
    """Цепочки происхождения (F12) полей комнаты IR.

    room — id комнаты из IR (обязателен); key — конкретный ключ provenance
    (без него — все ключи комнаты). file — слот.
    Ответ data: {room, entries:{key: "описание цепочки"}, session:{file}}.
    Ошибка not_found — нет такой комнаты или ключа.
    Пример: delve_provenance(room="hall").
    """
    return _call("provenance", _with_file({"room": room, "key": key}, file))


@mcp.tool()
def delve_asset_check(slot: str, asset: str, file: Optional[str] = None) -> dict:
    """Статическая проверка слот-ассета (.pgg) в контексте проекта (R-A3)
    + contract lint (A2).

    Отладка ассета без полного fill: интерфейс слота (входы/выходы/группы)
    сверяется с контрактом слота за миллисекунды; при чистой статике ассет
    дополнительно прогоняется один раз с минимальными синтетическими входами
    и схема выхода проверяется строго (delve/lint: нет групп/vec2/vec4,
    @style:int + @Cd:vec3 на точках меша кроме door, @kind:int на непустых
    якорях; падение синтетического прогона — warning «inconclusive», не
    ошибка). slot — вид слота (room_fill/wall_body/facing/door/...);
    asset — путь до .pgg (относительный резолвится сначала от assets/ delve,
    потом от asset_roots проекта). file — слот. Ответ data: {asset?
    (резолвнутый путь), diagnostics:[{code,message,...}] (delve/slot и
    delve/lint коды), has_errors, session:{file}}.
    Пример: delve_asset_check(slot="room_fill", asset="rooms/fill_v1.pgg").
    """
    return _call("asset_check", _with_file({"slot": slot, "asset": asset}, file))


# --- pgg layer: slot-asset debugging via PggServe ----------------------------


@mcp.tool()
def pgg_status() -> dict:
    """Живость PggServe (pgg-слой для отладки слот-ассетов).

    PggServe поднимается лениво — delve-инструменты его не трогают. При живом
    RPC — data: {serve:"running", binary?, rpc:{host,port}, slots, gpu,
    uptime_s, ...}. Бинаря нет — ok=false, error.kind=need_build (error.build —
    шаги сборки pgg, cwd = thirdparty/pgg). Порт 9878 (env PGG_SERVE_PORT).
    Пример: pgg_status().
    """
    return _pgg_layer.status()


@mcp.tool()
def pgg_load(path: str, lib_roots: Optional[list[str]] = None) -> dict:
    """Загрузить .pgg-ассет в слот PggServe (статическая проверка без прогона).

    path — абсолютный или относительно корня delve (``assets/walls/facing_v1.pgg``
    резолвится сам; прочие относительные — от корня pgg-репо, это cwd демона).
    lib_roots — корни import'ов; БЕЗ него подставляются автоматически: каталог
    ассета + <delve>/assets (+ каталог проекта и его asset_roots, если был
    delve_load) — этого хватает для ``import codes as c`` / ``import patterns
    as z`` delve-ассетов; сверху PggServe всегда дописывает свой resources/pgg.
    Относительные lib_roots — от корня delve. Ответ: {diagnostics:
    [{code,line,col,warning,message}], has_errors, ms, path, session:{file}}.
    После load ``file`` в pgg_params/pgg_render/pgg_probe/pgg_docs можно
    опускать. Слот-ассеты delve объявляют входы слота без дефолтов — load на
    них отвечает E604 (has_errors=true), это штатно: свяжите входы через
    pgg_params перед probe/render.
    Пример: pgg_load("assets/walls/facing_v1.pgg").
    """
    return _pgg_layer.load(path, lib_roots)


@mcp.tool()
def pgg_params(params: dict[str, Any], file: Optional[str] = None) -> dict:
    """Установить значения @param-параметров графа (переживают pgg_load).

    Слот-ассеты delve объявляют входы слота (seg/cuts/zones у facing и т.п.)
    без дефолтов — pgg_load на них отвечает E604; свяжите входы фикстурами
    перед probe/render: значение ``"@<файл>"`` грузит pgg-points/1 относительно
    каталога .pgg, например pgg_params({"seg": "@facing_v1.seg.points.json",
    "cuts": "@facing_v1.cuts.points.json", "zones": "@facing_v1.zones.points.json"}).
    Значения — числа/строки/bool или массив (сериализуется в вектор).
    Неизвестные имена — в поле unknown. file — слот; без него — последний
    pgg_load этого MCP-процесса. Ответ data: {params, unknown, session:{file}}.
    """
    return _pgg_layer.params(params, file=file)


@mcp.tool()
def pgg_probe(file: Optional[str] = None, spec: Optional[str] = None,
              specs: Optional[list[str]] = None) -> dict:
    """Пробник-инспектор узла загруженного .pgg (числа без картинки).

    spec — "путь:инспектор[параметры]", например "mesh:schema" или
    "mesh:bbox[group=stone]"; specs — список таких строк за ОДИН прогон
    (хотя бы один из spec/specs обязателен). Выходы delve слот-ассетов —
    ``mesh`` и ``anchors`` (контракт R-A3), промежуточные binding'и тоже
    доступны. file — слот; после pgg_load можно опускать. Правка .pgg на
    диске подхватывается сама (F4, reloaded=true).
    Ответ data: {records:[{origin,path,inspector,text}], diagnostics,
    has_errors, ms, cache:{hits,misses}, reloaded, session:{file}}.
    Пример: pgg_probe(spec="mesh:stats").
    """
    return _pgg_layer.probe(file=file, spec=spec, specs=specs)


@mcp.tool()
def pgg_render(node: str, file: Optional[str] = None, out: Optional[str] = None,
               size: Optional[list[float]] = None, ortho: Optional[str] = None,
               target: Optional[str] = None, orbit: Optional[list[float]] = None,
               zoom: Optional[float] = None) -> dict:
    """Кадр узла загруженного .pgg (GPU-рендер PggServe).

    node — имя binding/output'а. out — куда писать PNG (по умолчанию
    tmp/pgg_rpc_shots/ у pgg-репо); size — размер FBO; ortho — front|side|top|off;
    target — "x,y,z"|"group:<имя>"|"binding:<путь>"; orbit — [yaw, pitch];
    zoom — множитель fit-дистанции. Незаданные параметры — дефолты сервера
    (stateless). file — слот. Ответ data: {path?, width, height, stats,
    camera, render_state, cache, reloaded, session:{file}}.
    Без GPU (фоновая сессия, нет окна) — ok=false, kind=no_gpu: обходитесь
    pgg_probe + delve_asset_check.
    Пример: pgg_render(node="mesh", ortho="front") (у delve слот-ассетов
    выходы ``mesh``/``anchors``).
    """
    return _pgg_layer.render(node, file=file, out=out, size=size, ortho=ortho,
                             target=target, orbit=orbit, zoom=zoom)


@mcp.tool()
def pgg_docs(symbol: str, file: Optional[str] = None) -> dict:
    """Карточка def'а загруженного .pgg или builtin'а языка PGG.

    symbol — имя def'а слота или builtin'а (сначала def, при промахе —
    реестр билтинов; явный ``builtin:<name>`` — всегда реестр). file — слот.
    Ответ data: {symbol, kind, signature, docstring} (def) или {symbol, kind,
    name, signature, group, summary, example} (builtin); не найден — ok=false,
    kind=not_found + did-you-mean.
    Пример: pgg_docs("elem_zone"), pgg_docs("mesh_from_sdf").
    """
    return _pgg_layer.docs(symbol, file=file)


if __name__ == "__main__":
    mcp.run()
