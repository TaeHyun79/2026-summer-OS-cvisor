# cvisor

**English** | [한국어](#cvisor-한국어)

A ptrace-based execution visualizer for C programs on Linux x86-64.
It records *every instruction* of a target program's execution once, then lets
you step **forward and backward** through the recording in an ncurses TUI —
source, disassembly, registers, and memory (stack/heap/globals), side by side.

Built to accompany *Operating Systems: Three Easy Pieces* (OSTEP) and CSAPP:
the target environment, build flags, and on-screen layout are chosen so that
what you see matches the textbook figures 1:1.

```
┌─ showcase.c ─────────────┬─ disassembly ───────────────┐
│  5      if (n <= 1)      │  40113e: mov -0x4(%rbp),%eax│
│> 6          return 1;    │> 401141: cmp $0x1,%eax      │
├─ registers ──────────────┼─ stack ─────────────────────┤
│ RIP 0000000000401141     │ 7fffffffecb8 01 00 00 00    │
│ RAX 0000000000000001 *   │ 7fffffffecb0 ...      <-RBP │
│ FLAGS: CF0 ZF1 SF0 ...   │ (Tab: stack/heap/globals)   │
├──────────────────────────┴─────────────────────────────┤
│ step 128/3021 (+618 libc) | mode: SRC | </> step  q    │
└────────────────────────────────────────────────────────┘
```

Because execution is recorded first and browsed afterwards, stepping backward
is free — including **rewinding from a crash**: if the target dies with
SIGSEGV, you land in the TUI at the recording of its final moments.

## Requirements

- **Linux x86-64 only** (enforced at compile time). glibc recommended.
- `gcc`, `make`, `binutils` (objdump is used at runtime), `libncurses-dev`,
  `libdw-dev` (elfutils — variable names/types from DWARF).

### On an Apple Silicon Mac

The tool cannot run natively on macOS/arm64. Use an x86-64 Linux VM under
full-system emulation (e.g. [Lima](https://lima-vm.io) or UTM with QEMU TCG,
`arch: x86_64`) that mounts the project directory — edit code on the Mac,
build and run inside the VM shell. Note: Docker with Rosetta can compile the
code but **cannot record** (Rosetta does not support
`PTRACE_SINGLESTEP`/`GETREGS`); a real or emulated x86-64 kernel is required.
A native x86-64 Linux server over ssh works best if you have one.

## Build

```sh
make            # build cvisor (needs libncurses-dev, libdw-dev)
make tests      # build the example programs in tests/
make check      # quick smoke test
```

## Usage

The target program **must** be built with these flags (violations are
rejected at startup):

```sh
gcc -g -O0 -no-pie -fno-omit-frame-pointer -o target target.c
./cvisor ./target
```

Running cvisor does: (1) static analysis (objdump/ELF parsing) →
(2) one recorded execution ("Recording... N steps") → (3) the TUI.

| Option | Effect |
|---|---|
| `--dump` | print static analysis results and exit (no run) |
| `--trace` | record, then dump every step as text (no TUI) |
| `--from-main` | skip the crt/loader startup; record from `main` |
| `--max-steps N` | recording cap (default 200,000; marked "truncated") |

## TUI keys

| Key | Action |
|---|---|
| `→` / `n` | next step |
| `←` / `p` | previous step |
| `P` | switch process (fork children), time-synchronized via the global order |
| `m` | toggle step mode (instruction ↔ source line) |
| `Tab` | cycle memory pane (stack → heap → globals → rodata → code); in the wide layout it moves scroll focus instead |
| `↑` / `↓` | scroll the memory pane |
| `d` | toggle stack direction (default: high addresses on top, CSAPP style) |
| `g` | jump to a step number |
| `Home` / `End` | first / last step |
| `o` | program-output panel (shows output produced *up to this step*) |
| `s` | syscall log panel (number, args, return value, up to this step) |
| `v` | variables panel: locals/params of the current function + globals, with typed values |
| `f` | advance to the next call/ret instruction |
| `q` | quit |

### Layout

The layout adapts to the terminal size. On a large terminal (≥ 160 cols and
≥ 34 lines — roughly two-thirds of a typical fullscreen terminal; tunable via
`WIDE_MIN_COLS`/`WIDE_MIN_LINES` in `src/tui.c`) cvisor switches to a **4×2
grid** showing every memory section at once — source, disassembly, registers,
stack on top; heap, globals, rodata, code below — and `Tab` selects which
pane the `↑`/`↓` scroll keys act on (highlighted title). On smaller terminals
it uses the classic 2×2 layout with a single memory pane cycled by `Tab`.

### What the display tells you

- **Change highlighting**: registers (`*`) and memory bytes that differ from
  the previous step are shown in red — in both directions of travel.
- **EFLAGS** are decoded bit by bit (CF/PF/AF/ZF/SF/DF/OF) for studying
  conditional branches.
- **libc visibility**: libc internals are not recorded as steps, but the
  status bar shows how many hidden instructions ran, e.g. `(+2353 libc)`
  after a `malloc` — one visible step is not one instruction. Syscalls made
  inside that gap (e.g. the `write` behind `printf`) appear in the `s` panel.
- **Heap pane** shows the brk-based `[heap]` *and* anonymous rw `mmap`
  regions observed during recording (large mallocs), each with a header row.
- **All of the program image is browsable**: `globals` covers
  `.got/.data/.bss` (watch a GOT entry flip on the first `printf` — lazy
  binding, live), `rodata` shows string literals/constants, and `code` shows
  the raw `.text` machine bytes with a `<-RIP` marker following execution.
  Every hex pane has an ASCII column. (`.text`/`.rodata` are read-only
  mappings, loaded once from the ELF file.)
- **fork is followed**: every child gets its own step stream, and a global
  sequence number preserves the real interleaving. `P` switches between
  processes, landing on the step closest in time (so you can compare "where
  was the other process right now" — e.g. fork returning the pid in the
  parent and 0 in the child). **exec is followed too**: the new binary is
  re-analyzed on the fly; if it cannot be analyzed (PIE system binaries,
  no debug info) that process is marked "not followed past exec" and its
  trace ends at the exec.
- **Crashes**: on SIGSEGV etc. the faulting state is the last recorded step;
  press `End`, then walk backwards.

## Target constraints (Phase 1)

- `-g -O0 -no-pie` required (PIE binaries are rejected by an ELF check)
- single-threaded (fork/exec are followed; pthreads are not), single source
  file per binary; up to 8 processes / 8 exec'd binaries per recording
- addresses are stable across runs because ASLR is disabled for the child —
  a deliberate teaching setup, not how production systems behave

## Example programs (tests/)

| File | What to watch |
|---|---|
| `showcase.c` | **start here** — one short run that changes every pane, phase by phase: register/EFLAGS churn, recursion frames, `.bss` writes, brk-heap fills, a 1 MB `mmap` region appearing and vanishing, the GOT flip + `write` syscall on the first `printf` (see its header comment) |

## Project layout

```
src/cvisor.h    shared types (trace_t, step_t, ...) and tunables
src/analyzer.c  static analysis: ELF headers + objdump -d / decodedline parsing
src/recorder.c  ptrace engine: fork+TRACEME+ASLR off, SINGLESTEP loop, snapshots
src/trace.c     trace storage, binary-search lookups, register/syscall tables
src/dwarfvars.c variable names/types/locations from DWARF (libdw)
src/tui.c       ncurses panels, overlays, status bar
src/main.c      option parsing and orchestration
tests/          example/verification programs
```

## Roadmap

- [x] Phase 0 — static analysis (`--dump`)
- [x] Phase 1 — record engine (`--trace`), syscall capture, mmap-heap tracking
- [x] Phase 2 — TUI (MVP)
- [x] Phase 3a — follow-fork/exec: per-process step streams, global
      interleaving order, time-synchronized process switching (`P`)
- [x] Phase 3b — libdw variable display: green name/value annotations on the
      stack/globals hex rows plus the `v` variables panel (red on change)
- [ ] Phase 3 — libc-skip breakpoint optimization, PIE,
      diff encoding, threads (visualizing the `threads-intro/t1.c`
      race condition instruction-by-instruction is the long-term goal)

---

# cvisor (한국어)

리눅스 x86-64에서 C 프로그램의 실행을 ptrace로 **명령어 단위로 전부 기록**한 뒤,
ncurses TUI에서 소스/어셈블리/레지스터/메모리(스택·힙·전역)를 나란히 놓고
스텝 단위로 **앞뒤로** 탐색하는 도구입니다.

*Operating Systems: Three Easy Pieces*(OSTEP)와 CSAPP 공부용으로 만들어져,
기준 환경·빌드 플래그·화면 배치가 교재 그림과 1:1로 맞도록 설계되어 있습니다.

실행을 먼저 기록하고 나중에 탐색하는 구조라 뒤로가기가 공짜입니다 —
**크래시 되감기** 포함: 대상이 SIGSEGV로 죽으면 죽기 직전까지의 기록으로
TUI에 진입해 `End` → `←`로 원인을 거슬러 올라갈 수 있습니다.

## 요구 사항

- **Linux x86-64 전용** (다른 플랫폼은 컴파일 단계에서 거부). glibc 권장.
- `gcc`, `make`, `binutils`(런타임에 objdump 사용), `libncurses-dev`,
  `libdw-dev`(elfutils — DWARF에서 변수 이름/타입 추출).

### Apple Silicon 맥에서

macOS/arm64에서는 네이티브 실행이 불가능합니다. 프로젝트 폴더를 마운트하는
x86-64 리눅스 VM([Lima](https://lima-vm.io)나 UTM의 QEMU 전체 에뮬레이션,
`arch: x86_64`)을 사용하세요 — 코드는 맥에서 편집하고, 빌드/실행은 VM 셸에서
하면 됩니다. 참고: Docker+Rosetta는
컴파일은 되지만 `PTRACE_SINGLESTEP`을 지원하지 않아 **기록이 불가능**합니다.
학교/연구실의 진짜 x86-64 리눅스 서버가 있다면 그쪽이 가장 빠릅니다.

## 빌드

```sh
make            # cvisor 빌드 (libncurses-dev, libdw-dev 필요)
make tests      # tests/ 예제 프로그램 빌드
make check      # 스모크 테스트
```

## 사용법

대상 프로그램은 반드시 아래 플래그로 빌드해야 합니다 (위반 시 시작 단계에서 거부):

```sh
gcc -g -O0 -no-pie -fno-omit-frame-pointer -o target target.c
./cvisor ./target
```

실행하면 (1) 정적 분석 → (2) 기록 실행("Recording... N steps") → (3) TUI 진입.

| 옵션 | 동작 |
|---|---|
| `--dump` | 정적 분석 결과만 출력하고 종료 (실행 없음) |
| `--trace` | 기록 후 스텝별 텍스트 덤프 (TUI 없음) |
| `--from-main` | crt/로더 스타트업 생략, `main` 도달부터 기록 |
| `--max-steps N` | 기록 상한 (기본 200,000; 도달 시 truncated 표시) |

## TUI 키

| 키 | 동작 |
|---|---|
| `→` / `n` | 다음 스텝 |
| `←` / `p` | 이전 스텝 |
| `P` | 프로세스 전환 (fork 자식들) — 전역 순서 기준 시간 동기화 점프 |
| `m` | 스텝 모드 토글 (명령어 ↔ 소스 줄) |
| `Tab` | 메모리 패널 전환 (stack → heap → globals → rodata → code); 와이드 레이아웃에서는 스크롤 포커스 이동 |
| `↑` / `↓` | 메모리 패널 스크롤 |
| `d` | 스택 방향 토글 (기본: 높은 주소가 위 = CSAPP 방향) |
| `g` | 스텝 번호 입력 후 점프 |
| `Home` / `End` | 처음 / 마지막 스텝 |
| `o` | 프로그램 출력 패널 (현재 스텝까지의 출력만 표시) |
| `s` | 시스템 콜 로그 패널 (번호/인자/반환값, 현재 스텝까지) |
| `v` | 변수 패널: 현재 함수의 지역변수/인자 + 전역, 타입 해석된 값과 함께 |
| `f` | 다음 call/ret 명령어까지 전진 |
| `q` | 종료 |

### 레이아웃

터미널 크기에 따라 레이아웃이 자동으로 바뀝니다. 큰 터미널(가로 160컬럼·세로
34줄 이상 — 일반적인 풀스크린 터미널의 약 2/3 크기, `src/tui.c`의
`WIDE_MIN_COLS`/`WIDE_MIN_LINES`로 조절 가능)에서는 **4×2 그리드**로 전환되어
모든 메모리 섹션이 한 번에 보입니다 — 윗줄: 소스·디스어셈블리·레지스터·스택,
아랫줄: heap·globals·rodata·code. 이때 `Tab`은 `↑`/`↓` 스크롤이 적용될 패널을
선택합니다(타이틀 하이라이트). 그보다 작으면 기존 2×2 레이아웃에서 `Tab`으로
메모리 패널을 순환합니다.

### 화면 읽는 법

- **변경 강조**: 직전 스텝 대비 바뀐 레지스터(`*`)와 메모리 바이트가 빨간색으로
  표시됩니다 — 뒤로 갈 때도 동일 기준.
- **EFLAGS**는 CF/PF/AF/ZF/SF/DF/OF 비트별로 풀어서 표시 (조건 분기 학습용).
- **libc 가시성**: libc 내부는 스텝으로 기록되지 않지만, 그 구간에서 실행된
  명령어 수가 상태바에 `(+2353 libc)`처럼 표시됩니다 — 보이는 한 스텝이 명령어
  하나가 아닙니다. 그 사이의 시스템 콜(`printf` 뒤의 `write` 등)은 `s` 패널에서
  볼 수 있습니다.
- **힙 패널**은 brk 기반 `[heap]`과 기록 중 관측된 익명 rw `mmap` 영역(큰
  malloc)을 구역 헤더와 함께 보여줍니다.
- **프로그램 이미지 전체를 볼 수 있습니다**: `globals`는 `.got/.data/.bss`를
  포함하고(첫 `printf` 호출 때 GOT 엔트리가 바뀌는 lazy binding이 실시간으로
  보임), `rodata`는 문자열 리터럴/상수, `code`는 `.text`의 실제 기계어 바이트를
  `<-RIP` 마커와 함께 보여줍니다. 모든 hex 패널에 ASCII 컬럼이 있습니다.
  (`.text`/`.rodata`는 읽기 전용 매핑이라 ELF 파일에서 1회만 로드)
- **fork를 따라갑니다**: 자식마다 독립된 스텝 스트림이 생기고, 전역 순서번호가
  실제 인터리빙을 보존합니다. `P`로 프로세스를 전환하면 시간상 가장 가까운
  스텝으로 이동해 "그 순간 다른 프로세스는 어디였나"를 대조할 수 있습니다
  (fork가 부모에겐 pid, 자식에겐 0을 반환하는 장면 등). **exec도 따라갑니다**:
  새 바이너리를 즉석에서 재분석하며, 분석 불가(PIE 시스템 바이너리, 디버그 정보
  없음)이면 그 프로세스는 "exec 이후 미추적"으로 표시되고 트레이스가 exec에서
  끝납니다.
- **크래시**: SIGSEGV 등으로 죽으면 마지막 기록 스텝이 죽기 직전 상태입니다.
  `End`로 간 뒤 `←`로 되감으세요.

## 대상 프로그램 제약 (Phase 1)

- `-g -O0 -no-pie` 필수 (PIE는 ELF 검사로 거부)
- 단일 스레드 (fork/exec은 추적, pthread는 미지원), 바이너리당 단일 소스 파일;
  기록당 최대 8개 프로세스 / 8개 exec 바이너리
- 매 실행 같은 주소가 나오는 것은 자식 프로세스의 ASLR을 꺼서입니다 —
  학습용으로 의도된 설정이며, 실제 시스템 기본값과는 다릅니다

## 예제 프로그램 (tests/)

| 파일 | 관찰 포인트 |
|---|---|
| `showcase.c` | **여기서 시작** — 한 번의 짧은 실행으로 모든 패널이 단계별로 변함: 레지스터/EFLAGS 변화, 재귀 프레임, `.bss` 쓰기, brk 힙 채우기, 1MB `mmap` 영역의 등장과 소멸, 첫 `printf`의 GOT 플립 + `write` 시스템 콜 (파일 상단 주석 참조) |

## 코드 구조

```
src/cvisor.h    공용 타입 (trace_t, step_t, ...) 및 튜너블
src/analyzer.c  정적 분석: ELF 헤더 + objdump -d / decodedline 파싱
src/recorder.c  ptrace 엔진: fork+TRACEME+ASLR off, SINGLESTEP 루프, 스냅샷
src/trace.c     트레이스 저장, 이진탐색 조회, 레지스터/시스템콜 테이블
src/dwarfvars.c DWARF에서 변수 이름/타입/위치 추출 (libdw)
src/tui.c       ncurses 패널·오버레이·상태바
src/main.c      옵션 파싱 및 오케스트레이션
tests/          예제/검증 프로그램
```

## 로드맵

- [x] Phase 0 — 정적 분석 (`--dump`)
- [x] Phase 1 — 기록 엔진 (`--trace`), 시스템 콜 캡처, mmap 힙 추적
- [x] Phase 2 — TUI (MVP)
- [x] Phase 3a — follow-fork/exec: 프로세스별 스텝 스트림, 전역 인터리빙 순서,
      시간 동기화 프로세스 전환(`P`)
- [x] Phase 3b — libdw 변수 표시: 스택/globals hex 행의 초록 이름=값 주석 +
      `v` 변수 패널 (값 변경 시 빨강 강조)
- [ ] Phase 3 — libc 스킵 브레이크포인트 최적화, PIE 지원,
      디프 인코딩, 멀티스레드 (`threads-intro/t1.c`의 경쟁 조건을
      명령어 단위로 보여주는 것이 장기 목표)
