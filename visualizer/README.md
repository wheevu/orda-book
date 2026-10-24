# Order-by-order visualizer

This page steps through one price-time-priority example.
Its event-by-event book snapshots and trades come from `orda_replay --trace-json`, not a second matching implementation in JavaScript.
It is an explainer for the engine, not a market feed, and it never invents a timestamp: every axis is either price or event order.

A hosted copy is published at <https://wheevu.github.io/orda-book/>.
It is this page in static mode, so the parameter panel is not there.

The queue is what the example turns on.
At one price the oldest order fills first, and a change is a cancel and replace, so a modified order goes to the back of its queue even when its price and size do not change.
Before a fill that has a genuine choice in it, the page asks you to predict which resting order fills first, and it only asks when more than one resting order is actually in the queue.

## How the page is laid out

The transport sits above the workbench so stepping never needs a scroll to reach the control that steps.
It is one row inside one card: the buttons, the slider, the step counter, and the keyboard hint at the far end.
Below 660px it becomes two rows instead of three: the counter moves to the end of the slider row, and the hint is dropped, because a phone has no arrow keys and a third row of chrome is a third row the book is pushed down by.
The current event is not repeated there.
Between the tape's selected row, the selected card's title and its summary, a step is already stated three times, and the card's live region is what announces it.
Below it, a rail on the left holds the event tape, what the selected event did, and the fills it produced, and the order book takes the rest of the width.
Under 900px the rail dissolves and the page becomes one column in reading order: pick an event, read the book, then read what it did.

Under 900px the tape also turns sideways into one scrolling row of chips.
That is the difference between a tablet and a phone here.
Stacked above the book, the tape sat between the buttons and the ladder and pushed the ladder 391px below the control that steps it at 390px wide, which is most of a phone screen away from the thing the buttons change.
Sideways it is 96px instead of 194px, and the same distance is 249px, so the buttons, the steps and the whole ladder are on screen together at 844px tall and still at 667px.
The strip scrolls itself the shortest distance that brings the current chip, or the focused one, back into view, and it never scrolls the page: the reader is watching the book, and the book must not move under them.
Chrome's own focus scroll leaves a chip half cut in a horizontal scroller, so `focusin` on the tape does that scroll instead.
The strip carries its own padding because a scroller clips an outline, and a keyboard reader losing the focus ring on the step they are about to pick is not a trade worth making for 8px.

The selected and fills panels hold one height while the reader steps, and that height is measured rather than written down: `measureRailBoxes()` renders every step of the run once, keeps the tallest, and fixes the two boxes there, per run and per width.
A run with no fills does not reserve room for two receipts, and a narrow column does not reserve what the text needs on a wide one.
Hard-coding the worst case any run reaches kept the boxes still and left most of the page's vertical slack inside them.
The prediction block and the `−` caption under the ladder are revealed with `visibility` rather than shown with `display`, so neither changes its panel's content height.
Measured across every step of both runs from 1440px down to 320px, each panel holds one position and one height, and nothing clips.

Each resting order is a ticket in its own queue, numbered from 1, so the FIFO position the page is about is written out rather than implied by colour.
A ticket that gave up size on the current step carries a `−`.
An order the engine filled away entirely is not in the snapshot at all, so it appears only in the fills list; both lists use the same order ids.
A refused event is labelled `refused` in the tape, not only tinted.

A switch under the intro replays the counterfactual, because the whole point of the page is that the same four units fill in a different order once the older bid is re-sent, and that claim was previously visible only to someone running the dev server.
It swaps to a second checked-in trace, `data/queue-position.json`, so the hosted page gets the comparison without a matcher in JavaScript.
It is hidden on the dev server, where the parameter panel can already build both runs.
Both traces are fetched before the first render, so the switch is either on the page or not on it, rather than arriving under the reader's cursor.

The colour system is one accent.
Leaf green means "you chose this" or "this got filled": the selected event, the fills, and the bid side.
The ask side is graphite, so the two sides never both look like the accent and the layout carries the side without relying on colour.

`docs/DESIGN_THEME.md` records the palette, the type scale, the spacing, and the motion rules, so the next page starts from the same language instead of reinventing it.

## Motion

Every animation is decoration on top of a redraw, keyed to values already in the trace.
The depth edge draws outward from the price axis, a leaf sweep is drawn across the row where depth left the book, the previous profile fades as a ghost, and the history line grows with its newest dot arriving last.
The sweep and the ghost describe the change rather than the new state, so both are skipped entirely under `prefers-reduced-motion`; a shape that appears and vanishes without a fade is a flash, and the new profile, the `−` marks and the fills list already say what changed.
The draw-in is only replayed on a step, never on a resize, or the edges would flicker while a window is dragged.

## The two charts

**Cumulative depth by price.** Each price row is filled out to the total size resting from the touch out to that price, not the size at that level alone.
Bids grow leftward from the best bid and asks grow rightward from the best ask, so the two profiles never meet in the middle.
The price rows between the touch on each side are the spread, which the panel heading states as a price gap.
Price is the vertical axis and is shared with the ladder, drawn as a continuous band the bars grow out of, so a bar and the queue it belongs to are always on the same row.
The queue tickets live in their own columns either side of the chart, so a bar never covers its own queue, and the bands are trimmed so the value at the deepest tip has a gutter to sit in.
Below 660px the tickets win the extra width and the price axis narrows, and below 360px the position number is dropped; the order id, its size, and the queue's top-to-bottom order all survive.

Bar length is scaled to the deepest depth anywhere in the replay and never rescales between steps.
That is the whole point: a book that just lost four units has to look thinner.
`check-depth.mjs` asserts that scale, so a rescaled or inflated axis fails the build.

The ladder rows keep a 70px floor rather than a fixed height, because the number of orders resting at one price is real data.
70px is three orders, which is the most any shipped scenario reaches.
It was 66px until the rejection trace was measured on the page: three bids rest at 100 on its last step, which made one row grow by 4px and drag the caption, the history chart and the source section down with it.

**Cumulative depth by event.** A short step-line under the book showing how much size each side held after every event.
Its x axis spans the whole replay so it does not shift under the reader, but only the events already reached are drawn, so stepping does not hand over the answer early.

`trace.mjs` holds the arithmetic and nothing else, so the page and the test share one implementation.
Each bar's value stays in the DOM as `data-cumulative` and in the accessibility tree, because the visible label lives inside the SVG and screen readers skip that.

## The source under the book

The page's one dark band opens a section at the foot of the page, and that band is where the provenance claim lives: every number above came out of `lob::OrderBook`, and the page runs no matching logic of its own.

That section is not a card. It sits on the page the way the title and the intro do, because a card under a column of text puts a white canvas beside the text that the text does not fill, and an unfilled part of a white rectangle reads as dead space where the same amount of page beside the hero reads as margin.

Under it are five excerpts, each a claim the replay above demonstrates, written as a document rather than a grid of cards: a heading across the top, then a paragraph saying what the code is for on the left and the excerpt on the right with its source link underneath.

The section is on the page, not on a card, and the contrast is the point of that: an unfilled part of a white rectangle reads as dead space, where the same amount of page beside the title reads as margin. The excerpts are the exception, because code is something you look at inside a box. Argument on the ground, evidence in a box.

- a price level is a `std::list` in arrival order, so the queue is the list itself
- the levels sit in a `std::map` keyed so the best price is first on each side
- a new order is pushed to the back of its level, and a fill takes from the front
- a modify removes the order and adds it again, which is why the parameter panel's toggle reverses the fills

Every excerpt is verbatim, including the source indentation, and the line numbers are the ones in this repository.
Each one links to a permalink pinned to a commit rather than to `main`, so a quote and its link cannot drift apart: the quoted lines stay the lines the link opens, whatever happens to the file later.
`check-source.mjs` completes the other half, by re-reading `src/` and failing the build when the page and the source disagree.

Each excerpt is a `<figure>` with the source link as its `<figcaption>`, so the pair is one thing to a screen reader and one thing to lay out. The code column is a fixed width rather than sized to its content, which is what keeps every excerpt starting at the same x.

The excerpt text is plain in the markup and highlighted at load, so the file stays something a test can compare against `src/` and the highlight is decoration that can be wrong without changing a word. It also puts the text back if it ever does.

The last item in that section has no excerpt, because it is not a claim about any single line. It says why the book is a map of lists, what that costs, and which of the other two backends trades the cost differently, which is the part a reader cannot get from four lines of a header.

## Run it statically

The page falls back to the checked-in trace, which is what a static host gets.
The parameter panel stays hidden, because there is no engine to re-run the numbers.
`.github/workflows/pages.yml` publishes this folder to GitHub Pages on a push to `main` that touches it, minus the three check scripts and this README, and it fails the deploy rather than publishing a site missing either trace.

```sh
cmake -S . -B build-verify -DCMAKE_BUILD_TYPE=Debug
cmake --build build-verify --target orda_replay --parallel
python3 -m http.server 8000 --directory visualizer
```

Open <http://localhost:8000>.
The browser needs HTTP for the trace file and the ES module.
No build step, no dependencies.

## Run it with adjustable numbers

`scripts/serve_visualizer.py` adds one endpoint on top of the static server:

```
GET /api/trace?events=<url-encoded event text>
```

It writes the text to a temporary file, runs `orda_replay --trace-json` on it, and returns the engine's JSON.
With that endpoint reachable the page reveals the parameter panel, full width between the transport and the replay, and every change is replayed by the real matcher: sizes, the second bid's price, the sell's order type, and a toggle that modifies the older bid before the sell arrives.
That panel is not a rail card, because the book column cannot grow to match it and it left the whole right column empty below the book.
That last one is the counterfactual the page exists for.
Re-sending order 1 at the same price and size moves it behind order 2, so the same four units fill `2:2` then `1:2` instead of `1:3` then `2:1`.

```sh
cmake --build build-verify --target orda_replay --parallel
python3 scripts/serve_visualizer.py
```

Dev only. It binds to loopback, caps the scenario size, runs one fixed binary with no shell, and writes only to its own temporary directory.
Do not expose the server.
The page itself is safe to host statically, which is what the Pages workflow above does.
The default parameter values reproduce `scenarios/price-time-priority.events` exactly, so the first paint matches the checked-in fixture.

## Refused orders

An order the engine rejects is part of the story, so a trace frame records it: `accepted: false`, the error name, no trades, and the book exactly as it was.
`scenarios/rejections.events` covers a duplicate id, a crossing post-only, and a fill-or-kill with too little resting size.
The page marks those rows `refused` and explains the reason in the detail panel.

The export only fails for a parse error or the 500-event cap, never for a rejected event.
Both fields were already declared on every frame in the `replay-trace/v1` format, and no published trace predates it, so recording a refusal adds no new field and the version stays at `v1`.

## Refresh the example

After changing a scenario or the matcher, regenerate the trace from the engine:

```sh
cmake --build build-verify --target orda_replay --parallel
for scenario in price-time-priority rejections queue-position; do
  ./build-verify/orda_replay --trace-json "visualizer/scenarios/$scenario.events" > "visualizer/data/$scenario.json"
done
ctest --test-dir build-verify --output-on-failure
```

Seven tests cover this page.
`visualizer_trace_fixture`, `visualizer_rejection_fixture`, and `visualizer_queue_position_fixture` replay their source events and compare every exported frame with the checked-in data, and each checks that exporting more than 500 events fails instead of writing a partial trace.
`visualizer_depth_chart` recomputes both charts from the trace by a different route, a filtered sum instead of the module's outward walk, and checks that every refused event left the book and the trade log alone.
`visualizer_priority_order` asserts the lesson nothing else can: the same four units fill `1:3, 2:1` by default and `2:2, 1:2` once the older bid is modified, that the modify changed no price or size, that the fills consume the queue in order, and that the panel's default controls still build the checked scenarios.
`visualizer_script_syntax` runs `node --check` on `app.js`, which nothing else in the suite parses.
That check is only meaningful because of `package.json` in this folder, which marks it as ES modules: without it, `node --check` reads `app.js` as CommonJS, fails on the first `import`, and reports success for any error after it.
`visualizer_source_excerpts` checks the two ways a quote can go wrong, because they fail independently. It re-reads `src/` to catch a quote that has drifted from the working tree, and it reads the revision named in the link itself to catch a permalink that opens different lines from the ones the page shows. A permalink to the wrong commit resolves and highlights, so only the second comparison finds it.
That second comparison needs the commit in the clone. A shallow checkout cannot make it, and says so as a warning rather than passing quietly or failing a checkout that cannot answer.
The four Node checks are skipped when Node is absent, so a C++-only runner still passes.
Run them on their own with:

```sh
node visualizer/check-depth.mjs visualizer/data/price-time-priority.json visualizer/data/rejections.json
node visualizer/check-priority.mjs visualizer/data/price-time-priority.json visualizer/data/queue-position.json
node visualizer/check-source.mjs
```

Order IDs, prices, and quantities use decimal strings in JSON so browsers do not round 64-bit integers.
Trace export is capped at 500 events to keep snapshots bounded.
