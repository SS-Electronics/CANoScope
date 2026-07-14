# CANoScope

CANoScope is a Linux CAN bus monitoring, transmission, decoding, plotting, and
reverse-engineering application. It uses the Linux SocketCAN stack with a GTK 3
desktop interface, so it works with virtual CAN interfaces for development and
with CAN hardware exposed by the kernel as standard CAN network interfaces.

The application is built for daily CAN work: connect to an interface, watch live
traffic, send one-shot or cyclic frames, decode signals from a DBC file, plot
engineering values, compare two decoded signals statistically, reverse-engineer
unknown payloads, and create or update a DBC database from observed messages.

## Contents

- Features
- Quick Start
- Main Window
- Connection Settings
- Tab Guide
- Bit Analysis Theory
- Bit Analysis Workflow
- Software Architecture
- Build, Test, and Documentation
- Project Structure
- License

## Features

| Area | Capability |
|---|---|
| CAN traffic | Classic CAN, extended identifiers, CAN FD, remote frames, error frames, Rx and Tx display |
| Connection | SocketCAN interface selection, nominal bitrate, CAN FD data bitrate, listen-only mode, optional ID/mask filter |
| Trace | Live message table, unique-message roll-up, intervals, frequency, counts, CSV trace export |
| Transmission | Inline one-shot and cyclic transmit rows, dynamic CAN FD payload fields, advanced transmit dialog |
| DBC decoding | Load, reload, clear, and auto-populate decoded signal tables from DBC files |
| Graphing | Detached signal plot windows with per-signal color, live values, hover readout, and time-window control |
| Math | Two-signal statistical comparison with correlation, regression, error, and lag metrics |
| Bit Analysis | Targeted unknown-payload analysis with baseline, bit matrix, segment markers, candidate scanning, validation, and DBC promotion |
| DBC creation | Create or update message and signal definitions from observed frames or Bit Analysis candidates |
| Testing | Virtual CAN workflow, DBC traffic generator, focused Bit Analysis regression target |
| Documentation | Doxygen configuration for API and source reference |

## Quick Start

Install build/runtime dependencies:

```bash
sudo ./scripts/install_dependencies.sh
```

Build the application:

```bash
make
```

Run it:

```bash
./build/canoscope
```

Create a virtual CAN interface for off-hardware testing:

```bash
sudo modprobe can
sudo modprobe can_raw
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan 2>/dev/null || true
sudo ip link set dev vcan0 up
```

Connect CANoScope with `File > Connect`, select `vcan0`, keep the nominal
bitrate at a normal test value such as `500 kbit/s`, and press `Connect`.

Send a simple frame from another terminal:

```bash
cansend vcan0 123#DEADBEEF
```

Generate decoded DBC traffic from the bundled demo database:

```bash
python3 scripts/test_dbc.py --no-setup --rate 30 --duration 60
```

## Main Window

CANoScope opens maximized and the notebook tabs resize with the main window.
The top-level layout is:

| Region | Purpose |
|---|---|
| Menu bar | File, Trace, CAN, View, Database, and Help actions |
| Statistics bar | Always-visible connection and traffic state |
| Toolbar | Fast access to common connect, disconnect, record, stop, save, and settings actions |
| Tab notebook | Receive / Transmit, Bit Analysis, Signal Analysis, Signal Analysis Viewer, Math, and DB Creation |

### Statistics Bar

| Field | Meaning |
|---|---|
| Interface | Active SocketCAN interface, or `-` when disconnected |
| Bitrate | Configured nominal bitrate |
| Bus Load | Approximate load percentage with a progress bar |
| Rx | Received frame count |
| Tx | Transmitted frame count |
| Err | Error-frame count |
| State | Driver-reported bus state: Active, Warning, Passive, or Bus-Off |

### Menus

| Menu | User operations |
|---|---|
| File | Connect, disconnect, quit |
| Trace | Start capture, stop capture, save captured frames as CSV |
| CAN | Open connection/settings actions and CAN-related controls |
| View | Toggle presentation modes exposed by the GUI |
| Database | Load a DBC, load the bundled demo DBC, clear the active database |
| Help | About and application information |

## Connection Settings

Open with `File > Connect` or the connect toolbar action.

| Field or button | What it does | Choose it when |
|---|---|---|
| Interface | Selects or types the SocketCAN interface name, such as `vcan0` or `can0` | You need to choose the bus to monitor |
| Refresh icon | Re-scans available CAN interfaces | An interface was plugged in or created after the dialog opened |
| Nominal Bitrate | Sets the arbitration bitrate; presets are available and custom values can be typed | Connecting to a real bus that requires a specific bitrate |
| Listen Only | Opens the interface passively | You want to observe traffic without actively participating in bus acknowledgement |
| Enable CAN FD | Enables CAN FD support for receive and transmit paths | The bus carries CAN FD frames or you need to send FD payloads |
| Data Bitrate (FD) | Sets CAN FD data-phase bitrate | CAN FD is enabled and the bus uses a separate data bitrate |
| Enable acceptance filter | Enables kernel-level ID/mask filtering | You only want frames matching one identifier pattern |
| ID 0x | Filter identifier in hexadecimal | Filtering one message or a message group |
| Mask 0x | Filter mask in hexadecimal; compared as `(id & mask) == (filter & mask)` | Filtering a range or exact identifier |
| Ext | Applies the filter to extended identifiers | The filtered message uses a 29-bit identifier |
| Connect | Applies settings and opens the interface | Ready to start live operation |
| Cancel | Closes the dialog without changing the active connection | You do not want to connect |

## Tab Guide

### Receive / Transmit

Use this tab for live bus inspection and manual frame transmission.

#### Receive Trace

| Column | Meaning |
|---|---|
| # | Row number. In roll-up mode this is the unique message row number |
| Dir | `Rx` for received frames or `Tx` for frames sent by CANoScope |
| Timestamp | Frame timestamp |
| ID | CAN identifier formatted as standard or extended |
| Type | Frame type: standard, extended, CAN FD, CAN FD with bitrate switch, remote, or error |
| DLC | Data length code |
| Data | Payload bytes in hexadecimal |
| Interval (ms) | Time since the previous matching frame |
| Freq (Hz) | Estimated update frequency for the matching frame |
| Count | Number of times this row's message has been seen |

#### Transmit Messages Panel

| Field or button | What it does | Choose it when |
|---|---|---|
| Row number | Identifies the transmit row | Managing multiple transmit frames |
| ID | Hexadecimal identifier to send | You know the target message identifier |
| Ext | Sends a 29-bit extended identifier | The message uses extended frame format |
| RTR | Sends a remote request frame | The bus protocol expects a remote request |
| DLC | Payload length; grows up to 64 bytes when CAN FD is enabled | Selecting how many data bytes are sent |
| Data bytes | Hexadecimal payload bytes | Setting frame content |
| Interval | Cycle period in milliseconds | Sending periodic traffic |
| Send | Sends the row once | Testing one frame or triggering a one-shot command |
| Start | Starts cyclic transmission for that row | Simulating a periodic ECU message |
| Stop | Stops cyclic transmission for that row | Ending a periodic transmit task |
| Status | Shows row state or error text | Checking whether a send or cyclic task succeeded |
| Add Row | Adds another transmit row | You need several independent messages |
| Remove Last | Removes the last transmit row | Cleaning up unused rows |
| Clear Rows | Clears transmit rows back to the default state | Starting a new transmit setup |
| Advanced TX... | Opens the detailed transmit dialog | You want a larger form for one-shot or cyclic transmission |

#### Advanced Transmit Dialog

| Section | Fields and buttons |
|---|---|
| Frame | ID, extended flag, remote flag, CAN FD options, DLC |
| Data (hex) | Data byte entries, shown according to the selected DLC |
| Cyclic Transmit | Interval, Start Cyclic, Stop Cyclic |
| Footer buttons | Send Once and Close |

Use inline rows for repeated day-to-day traffic. Use the advanced dialog when
you want a focused single-message transmit form.

### Bit Analysis

Use this tab when you do not yet have a reliable DBC definition for a message.
It turns controlled observations into candidate signal definitions.

The tab has five working areas:

1. Target and session controls.
2. Manual input and experiment segment controls.
3. Live CAN ID survey and segment table.
4. 64-bit matrix, selected-field timeline, and manual field inspector.
5. Candidate table and candidate inspector.

#### Target and Session Controls

| Field or button | What it does | Choose it when |
|---|---|---|
| Target | Selects the CAN message to analyze from observed traffic | You have identified the message carrying the unknown value |
| Dir | Filters target frames by direction: RX, TX, or Both | You only trust received frames, transmitted frames, or both |
| DLC | Expected payload length | The target message has a stable length |
| Varying DLC | Accepts target frames even when DLC changes | The message length is not stable or you are studying a mixed stream |
| FD full payload | Analyzes the full CAN FD payload instead of only the classic 8-byte area | The unknown field may be beyond byte 7 |
| Start | Starts capturing target frames into the analysis session | You are ready to collect samples |
| Pause/Resume | Temporarily stops or resumes capture without deleting data | You need to adjust the physical input or inspect the current state |
| Reset | Clears samples, markers, baseline, and candidates for the current session | Starting the target analysis over |
| Capture Baseline | Computes the baseline bit pattern from baseline segments, or from all samples if no baseline segment exists | You have captured a known idle/reference state |
| Analyze | Computes bit statistics and ranked candidate fields | You have enough samples and input markers |
| Save Session | Writes samples, segments, baseline, candidates, and settings to JSON | You need to continue later or share the evidence |
| Load Session | Restores a saved JSON analysis session | You want to review or continue previous work |

#### Input and Segment Controls

| Field or button | What it does | Choose it when |
|---|---|---|
| Input name | Human-readable name for the controlled physical input | Naming the thing you changed, such as speed, switch, position, or temperature |
| Input type | Boolean, Enumerated, Continuous, or Event | Selecting the analysis model for your experiment |
| Input value | Numeric value assigned to new samples and segments | Correlating payload fields to a known input value |
| Unit | Engineering unit for the input value | You know the physical unit, such as rpm, km/h, degC, bar, or percent |
| Segment type | Baseline, Static, Toggle, Step, Ramp, Boundary, Validation, or Event | Telling the analyzer how to interpret this part of the experiment |
| Segment label | Optional name for the marker | Recording what you did physically |
| Add Marker | Starts a new segment with the current input settings | Marking the beginning of a controlled state or motion |
| End Segment | Closes the active segment | Marking the end of the controlled period |

#### Input Type Selection

| Input type | Meaning | Best use |
|---|---|---|
| Boolean | The input has two states, normally 0 and 1 | Switches, brake state, enable flags, door open/closed |
| Enumerated | The input is one of several named or numbered states | Gear, mode, selector position |
| Continuous | The input is numeric and ordered | Speed, pressure, temperature, position, load |
| Event | The input is a moment, not a sustained numeric value | Button press, edge trigger, intermittent action |

#### Segment Type Selection

| Segment type | Meaning | Best use |
|---|---|---|
| Baseline | Known reference state used to build the baseline bit pattern | Ignition on idle, actuator off, zero position |
| Static | Input held at one stable value | Measuring fields that settle at a known level |
| Toggle | Boolean input changing between two states | Finding switch bits and transition-related fields |
| Step | Input moves between discrete numeric levels | Estimating scale and offset from known values |
| Ramp | Input changes gradually | Finding monotonic fields and time lag |
| Boundary | Input is driven to min, max, or known limits | Confirming bit length, signedness, and saturation |
| Validation | Independent data not used as the primary discovery set | Checking whether a candidate generalizes |
| Event | A short event marker | Finding one-shot flags or response windows |

#### Status Summary

The one-line summary reports:

| Item | Meaning |
|---|---|
| State | Idle, ready, capturing, paused, analyzing, or review |
| Target | Selected identifier, frame format, and DLC |
| Samples | Number of stored target samples versus session capacity |
| Dropped | Samples lost when the ring buffer capacity was exceeded |
| Baseline | Whether a baseline has been captured |
| Candidates | Number of ranked candidate fields currently available |

#### Live CAN ID Survey

| Column | Meaning |
|---|---|
| Message | Observed identifier, frame format, and DLC |
| Mean | Mean period between frames |
| Jitter | Period variation |
| Bits | Count of payload bits observed changing |
| Change | Rate or amount of change in the payload |
| Unique | Count of unique payload values seen |

Use the survey to choose the target message. Periodic messages with changing
bits and a reasonable unique-value count are often good reverse-engineering
targets.

#### Experiment Segments

| Column | Meaning |
|---|---|
| # | Segment number |
| Type | Segment type |
| Label | User label |
| Input | Input value and unit |
| Frames | Number of captured target frames inside the segment |

Good segment hygiene matters. Keep segments short, intentional, and labeled.
Separate discovery data from validation data.

#### Manual Field Inspector

| Field or button | What it does |
|---|---|
| Field start | Canonical start bit to inspect |
| Len | Bit length to inspect |
| Intel/Motorola | Byte-order interpretation |
| Signed | Treats the extracted raw value as two's-complement signed |
| Raw | Current raw extracted value from the latest payload |
| Fit | Current fitted physical value when factor/offset evidence exists |
| Mark Validated | Runs validation on the selected candidate against validation segments |
| Promote to DB Creation | Copies the selected candidate into DB Creation for final review and saving |

Use the manual inspector when you already suspect a field location or need to
confirm a candidate by changing start, length, byte order, or signedness.

#### 64-bit Matrix

The matrix displays payload bits in byte rows and bit columns. Click a cell to
select a bit and update the bit statistics label.

| Mode | What it shows | Use it for |
|---|---|---|
| Live | Current bit value from the latest target frame | Seeing present payload state |
| Baseline | Baseline bit value | Confirming the reference pattern |
| Baseline XOR | Difference between live bits and baseline bits | Finding bits affected by a change from reference |
| Activity | Flip/change activity | Finding counters, timers, and active signals |
| Entropy | How balanced 0/1 values are | Finding information-rich bits |
| Correlation | Relationship between bit value and input value | Finding bits connected to the controlled input |

The bit statistics label includes the selected bit, live and baseline values,
flip count, probability, entropy, state separation, correlation, best lag, and
lagged score where enough data exists.

#### Selected Field Timeline

The timeline plots the currently selected field over time. Use it to check
whether a candidate moves when the physical input moves, whether it lags the
input, and whether it resets or wraps.

#### Candidate Fields

| Column | Meaning |
|---|---|
| # | Rank in the candidate list |
| Start | Canonical start bit |
| DBC | DBC start bit for the selected byte order |
| Len | Field length in bits |
| Order | Intel or Motorola byte order |
| Type | Unsigned or signed interpretation |
| r | Pearson linear correlation with the input |
| rho | Spearman rank/monotonic correlation |
| R2 | Regression coefficient of determination |
| Factor | Estimated physical scaling factor |
| Offset | Estimated physical offset |
| MAE | Mean absolute fit error on discovery data |
| ValN | Number of validation samples used |
| ValMAE | Mean absolute error on validation data |
| Lag | Best estimated response lag in milliseconds |
| Trans | Matched transitions for Boolean input experiments |
| Counter | Counter-likeness score |
| Time | Timestamp-likeness score |
| TimeRes | Estimated timestamp resolution in milliseconds |
| Checksum | Checksum-likeness score |
| Mux | Multiplexer-likeness score |
| MuxVals | Number of observed multiplexer values |
| MuxMin | Minimum frames observed per multiplexer value |
| MuxBits | Count of bits that appear conditionally active |
| MuxRanges | Bit ranges that appear conditionally active |
| Score | Overall ranking score |
| Confidence | Classification such as Probable, Candidate, Counter, Timestamp-like, Checksum-like, Mux-like, or Unknown |

Select a candidate to update the inspector and timeline.

#### Candidate Inspector

The inspector expands the selected row into readable evidence: proposed signal
name, raw range, physical range, scaling, error metrics, validation results,
lag, metadata scores, multiplexer evidence, and notes. Use it before promoting
a candidate into the DBC workflow.

### Signal Analysis

Use this tab when you have a DBC and want decoded signal values.

| Control | What it does |
|---|---|
| Load DBC... | Opens a file chooser and loads a database |
| Reload | Reloads the current database path |
| Clear | Unloads the active database and clears the signal table |
| Database label | Shows the active database name plus message and signal counts |

| Column | Meaning |
|---|---|
| Message | DBC message name |
| ID | Message identifier |
| Signal | DBC signal name |
| Raw | Latest unscaled raw field value |
| Value | Latest physical value after factor/offset scaling |
| Unit | Engineering unit from the DBC |
| Range | Documented min/max from the DBC |
| Count | Number of updates received for that signal |

Choose Signal Analysis when you need table-style monitoring of decoded values.
It is the fastest way to confirm that a DBC matches live traffic.

### Signal Analysis Viewer

Use this tab for oscilloscope-style graphing of decoded DBC signals.

#### Launcher Tab

| Control | What it does |
|---|---|
| Add Analysis Window | Opens a detached plotting window |
| Load DBC... | Loads or replaces the active database |
| Analysis window count | Shows how many detached graph windows are open |

#### Detached Analysis Window

| Control | What it does | Choose it when |
|---|---|---|
| Signal | Searchable signal dropdown using `Message.Signal` labels | Selecting a decoded signal to plot |
| Add | Adds the selected signal to the graph | Building a trace set |
| Clear | Removes all signals from that graph window | Starting the plot over |
| Pause | Freezes the graph without closing the window | Inspecting a time slice |
| Window (s) | Visible time span in seconds | Zooming the time axis |
| Side pane | Lists plotted signals, colors, and live values | Reading current values without hovering |
| Remove button in side pane | Removes one plotted signal | Cleaning up a graph without clearing all signals |
| Graph hover | Shows the nearest sample value, signal, unit, and time | Inspecting a precise point |

Choose Signal Analysis Viewer when trends matter more than table values. Open
multiple detached windows to compare independent signal groups.

### Math

Use this tab to compare two decoded signals numerically.

| Control or field | What it does |
|---|---|
| X | Selects the independent signal |
| Y | Selects the dependent signal |
| Window (s) | Sets how much recent data is included in calculations |
| Clear | Clears collected X/Y samples |
| Samples | Count of paired samples currently included |
| Signals vs Time | Time plot of the selected signals |
| Y vs X | Scatter plot of Y against X |

The statistics label reports:

| Metric | Meaning |
|---|---|
| X/Y mean, std, min, max | Basic descriptive statistics |
| Cov | Covariance between X and Y |
| Pearson | Linear correlation |
| Spearman | Rank/monotonic correlation |
| Best lag | Sample offset with strongest correlation |
| Regression | Fitted line `y = intercept + slope * x` |
| R2 | Fit quality for the regression |
| MAE | Mean absolute regression error |
| RMSE | Root mean squared regression error |

Choose Math when you suspect one signal derives from another, lags another, or
should have a known linear relationship.

### DB Creation

Use this tab to create or update DBC message/signal definitions from observed
traffic or promoted Bit Analysis candidates.

#### Target Database

| Control | What it does |
|---|---|
| Target path | Destination DBC path |
| New... | Chooses a path for a new database |
| Open... | Opens an existing DBC for editing |
| Use Loaded | Uses the database currently loaded in Signal Analysis |

#### Source RX Message

| Field or button | What it does |
|---|---|
| Message ID | Selects an observed received message |
| Refresh | Rebuilds the observed-message list |
| RX message count | Shows how many received messages are available |
| Latest RX | Shows the latest payload for the selected message |

#### Message and Signal

| Field | Meaning |
|---|---|
| Message name | DBC message name to create or update |
| Signal name | DBC signal name |
| Start bit | DBC start bit |
| Length | Signal length in bits |
| Byte order | Intel little-endian or Motorola big-endian |
| Signed | Signed two's-complement interpretation |
| Factor | Physical scaling factor |
| Offset | Physical offset |
| Min | Documented minimum physical value |
| Max | Documented maximum physical value |
| Unit | Engineering unit |
| Comment | Optional DBC signal comment; promoted Bit Analysis evidence is inserted here |
| Sample raw | Raw value extracted from the latest selected payload |
| Sample value | Physical value computed from sample raw, factor, and offset |
| Sample Value vs Time | Preview plot of the extracted field over recent samples |
| Create / Update Signal | Writes the message and signal definition into the target database |
| Status | Result or error message for the save operation |

Choose DB Creation after you know enough about a signal to save it. Bit
Analysis promotion fills many fields automatically, but the user still reviews
and saves the final DBC entry.

## Bit Analysis Theory

Bit Analysis is based on controlled experiments. CAN payloads are opaque bytes,
but many physical signals produce measurable patterns when the user changes one
input at a time.

### Data Model

The engine stores accepted target frames in a preallocated ring buffer. Each
sample contains timestamp, sequence number, DLC, payload bytes, direction, the
current manual input value, and the active segment id. This lets the analyzer
compare payload changes against known user actions.

Only frames matching the selected target ID, direction, and DLC policy are
accepted. This keeps unrelated traffic out of the analysis.

### Canonical Bits and Byte Order

CANoScope displays bits in canonical byte/bit coordinates: byte 0 bit 0 is the
least significant bit of the first payload byte, byte 0 bit 7 is the most
significant bit of the first byte, and byte 1 starts at bit 8.

When scanning fields, the engine tests both byte orders:

| Order | Interpretation |
|---|---|
| Intel | Consecutive bits grow upward in canonical bit order |
| Motorola | Bits follow DBC big-endian signal ordering |

For each field, the engine also tests unsigned and signed interpretations.
Signed candidates use two's-complement sign extension.

### Baseline

A baseline is the reference payload state. If baseline segments exist, the
baseline is built from those samples. Otherwise, it is built from all current
samples. For each bit, the baseline value is the majority value across the
baseline samples.

Baseline XOR mode highlights bits whose current value differs from that
reference. This is useful for finding the bits affected by a controlled change.

### Per-Bit Statistics

For every bit, the analyzer computes:

| Statistic | Meaning |
|---|---|
| one/zero count | How often the bit is 1 or 0 |
| probability one | Fraction of samples where the bit is 1 |
| flip count | Number of bit transitions |
| flip rate | Transitions per second |
| entropy | Information content; near 0 means constant, near 1 means balanced |
| baseline difference rate | Fraction of samples that differ from baseline |
| state separation | Difference between bit behavior at different input states |
| Pearson correlation | Linear relation between bit value and input value |
| best lag | Delay that improves correlation with the input |
| lagged score | Correlation score at the best lag |

Constant bits are usually configuration or unused padding. Highly active bits
can be counters, timers, checksums, multiplexers, or real signals. Correlation
and segment context help separate those cases.

### Candidate Field Scanning

The analyzer scans contiguous fields and extracts raw values for each possible
start bit, length, byte order, and signedness that fits inside the selected
payload size.

For each candidate it computes:

| Metric | Purpose |
|---|---|
| raw min/max | Determines observed field range |
| unique raw values | Distinguishes constant, stepped, and continuously changing fields |
| factor/offset | Fits raw values to the user's input values |
| physical min/max | Estimated physical range |
| Pearson | Linear relationship to the input |
| Spearman | Monotonic relationship to the input |
| R2 | Regression fit quality |
| MAE and max error | Fit error against known input values |
| lag | Delay between input and payload response |
| transition score | Boolean transition match quality |
| total score | Combined ranking score |

Continuous and step experiments favor correlation, monotonicity, regression
quality, and low error. Boolean experiments favor state separation and
transition matches. Event experiments are better for finding response windows
than for fitting scale.

### Protocol Metadata Detection

Not every changing field is a physical signal. The engine classifies common
metadata patterns:

| Pattern | Evidence |
|---|---|
| Counter | Small field, repeated incrementing sequence, wrap behavior |
| Timestamp-like | Monotonic growth with time and low dependency on user input |
| Checksum-like | High entropy, low input correlation, not counter-like |
| Multiplexer-like | Small selector value where other bit ranges become conditionally active |

These classifications protect the user from promoting counters or checksums as
physical signals. They also help document useful protocol structure.

### Validation

Validation uses samples from `Validation` segments. A candidate discovered from
baseline, static, step, ramp, toggle, boundary, or event data should still fit
independent validation data. The Mark Validated action updates validation
sample count and validation error metrics.

High-confidence work usually follows this rule:

1. Collect discovery segments.
2. Analyze and inspect candidates.
3. Collect separate validation segments.
4. Validate the candidate.
5. Promote it to DB Creation.
6. Review and save the DBC signal.

## Bit Analysis Workflow

1. Connect to the bus. Use listen-only mode when you are observing an unknown
   or shared bus.
2. Watch the Live CAN ID Survey. Pick a message that changes when your physical
   input changes.
3. Select the target, direction, and DLC policy.
4. Press Start.
5. Capture a Baseline while the system is in a known reference state.
6. Add clean experiment segments. Change one physical input at a time.
7. Use matrix modes to inspect bit behavior.
8. Press Analyze.
9. Sort candidates by confidence and score. Inspect raw range, fit, lag, and
   metadata scores.
10. Add a Validation segment with fresh data.
11. Press Mark Validated on the candidate.
12. Press Promote to DB Creation.
13. Review message name, signal name, start bit, length, byte order,
   signedness, factor, offset, range, unit, and comment.
14. Press Create / Update Signal.

## Software Architecture

CANoScope separates hardware I/O, application state, analysis logic, DBC logic,
and GTK widgets. The analysis and DBC modules are GTK-independent so they can
be tested without a display server.

### Runtime Layers

```text
GTK application
    |
    v
GUI controllers and shared app state
    |
    v
Generic CAN driver interface
    |
    v
SocketCAN backend
    |
    v
Linux PF_CAN raw socket
    |
    v
SocketCAN network interface
```

### Major Modules

| Module | Responsibility |
|---|---|
| `main.c` | GTK application entry point, version handling, startup |
| `inc/can_message.h` | CAN message and statistics types |
| `inc/app_state.h` | Shared application state |
| `inc/drv_can.h`, `driver/drv_can.c` | Generic CAN driver vtable and wrapper |
| `inc/socketcan.h`, `driver/socketcan.c` | SocketCAN implementation |
| `inc/dbc.h`, `driver/dbc.c` | DBC model, parser, writer, raw extraction, physical decoding |
| `inc/bit_analysis.h`, `analysis/bit_analysis.c` | Bit Analysis data model, statistics, candidate engine, validation, JSON sessions |
| `inc/gui.h` | GUI declarations and shared widget handles |
| `gui/main_window.c` | Main window, menu, toolbar, notebook, inline transmit panel |
| `gui/message_view.c` | Receive trace table, row formatting, bus state presentation |
| `gui/threads.c` | Connection lifecycle, receive thread, transmit queue, stats updates |
| `gui/settings_dialog.c` | Connection settings dialog |
| `gui/bit_analysis_view.c` | Bit Analysis tab, matrix/timeline drawing, session coordination |
| `gui/signal_view.c` | Signal Analysis tab |
| `gui/signal_plot.c` | Detached graph windows for Signal Analysis Viewer |
| `gui/math_view.c` | Two-signal math tab |
| `gui/db_creation.c` | DBC creation/update workflow and Bit Analysis promotion target |
| `gui/transmit_dialog.c` | Advanced transmit dialog |

### Threading Model

```text
Receive thread
    reads CAN frames
    schedules GTK-safe updates through the main loop

Transmit thread
    consumes queued transmit requests
    writes frames through the driver backend

Statistics thread
    polls driver statistics every 500 ms
    schedules statistics-bar updates

GTK main thread
    owns every widget
    receives idle callbacks from worker threads
```

GTK widgets are updated only from the GTK main thread. Worker threads pass data
back using GLib queues and idle callbacks.

### Receive Data Flow

1. The connection dialog configures interface, bitrate, mode, CAN FD, and
   optional filter.
2. The driver opens a PF_CAN raw socket.
3. The receive thread waits for frames.
4. Each frame is normalized into `can_msg_t`.
5. The GTK main thread updates:
   - Receive trace.
   - Signal Analysis decoded values.
   - Signal Analysis Viewer plots.
   - Math paired samples.
   - Bit Analysis target sample ring.
   - DB Creation observed-message cache.

### Transmit Data Flow

1. The user presses Send, Start, Stop, or a cyclic action.
2. The GUI builds a `can_msg_t`.
3. One-shot sends are queued immediately.
4. Cyclic rows schedule periodic enqueue events.
5. The transmit thread writes frames through the driver.
6. Sent frames are reflected in the trace as Tx rows.

### DBC Data Flow

1. `dbc_load_file()` parses messages and signals.
2. Signal Analysis rebuilds the signal table.
3. Signal Analysis Viewer and Math receive the same active database.
4. Incoming frames are matched by identifier and frame format.
5. `dbc_extract_raw()` extracts each signal field.
6. `dbc_decode_physical()` applies signedness, factor, and offset.
7. DB Creation can update or write the database back to disk.

### Bit Analysis Data Flow

1. The user selects a target frame.
2. Matching samples enter the Bit Analysis ring buffer.
3. The user adds segments and input values.
4. Baseline capture creates a reference bit pattern.
5. Analyze computes per-bit statistics and field candidates.
6. Validation tests candidates against validation segments.
7. Promotion copies the chosen candidate into DB Creation.
8. DB Creation performs the final DBC write.

### File Outputs

| Output | Format | Created by |
|---|---|---|
| Trace capture | CSV | Trace menu |
| CAN database | DBC | DB Creation |
| Bit Analysis session | JSON | Bit Analysis Save Session |
| API documentation | HTML | Doxygen via `make docs` |

## Build, Test, and Documentation

### Dependencies

| Package | Purpose |
|---|---|
| `gcc` and `make` | C build toolchain |
| `pkg-config` | Build flags for dependencies |
| `libgtk-3-dev` | GTK 3 user interface |
| `libglib2.0-dev` | GLib queues, timers, and threading utilities |
| `can-utils` | Command-line CAN test tools such as `cansend` |
| `iproute2` | CAN interface setup through `ip link` |
| `python3` | Test traffic scripts |
| `doxygen` and `graphviz` | Optional API documentation generation |

### Build Commands

```bash
make
make DEBUG=1
sudo make install
sudo make uninstall
```

`make` produces `build/canoscope`.

### Virtual CAN Test Commands

```bash
sudo modprobe can
sudo modprobe can_raw
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan 2>/dev/null || true
sudo ip link set dev vcan0 up

cansend vcan0 123#DEADBEEF
python3 scripts/test_dbc.py --no-setup --rate 30 --duration 30
```

### Automated Checks

```bash
make
make test-bit-analysis
python3 scripts/test_dbc.py --no-setup --duration 1 --rate 20
git diff --check
```

### API Documentation

```bash
sudo apt-get install -y doxygen graphviz
make docs
xdg-open docs/index.html
```

The generated documentation is written to `docs/index.html`.

## Project Structure

```text
CANoScope/
|-- main.c
|-- Makefile
|-- inc/
|   |-- app_state.h
|   |-- bit_analysis.h
|   |-- can_message.h
|   |-- dbc.h
|   |-- drv_can.h
|   |-- gui.h
|   `-- socketcan.h
|-- analysis/
|   `-- bit_analysis.c
|-- driver/
|   |-- dbc.c
|   |-- drv_can.c
|   `-- socketcan.c
|-- gui/
|   |-- bit_analysis_view.c
|   |-- db_creation.c
|   |-- main_window.c
|   |-- math_view.c
|   |-- message_view.c
|   |-- settings_dialog.c
|   |-- signal_plot.c
|   |-- signal_view.c
|   |-- threads.c
|   `-- transmit_dialog.c
|-- assets/
|   |-- canoscope.desktop
|   |-- canoscope.png
|   |-- demo.dbc
|   `-- taksys_logo.png
|-- scripts/
|   |-- install_dependencies.sh
|   `-- test_dbc.py
|-- tests/
|   `-- test_bit_analysis.c
|-- debian/
|-- docs/
|-- Doxyfile
`-- README.md
```

## License

Apache License 2.0. See `LICENSE`.
