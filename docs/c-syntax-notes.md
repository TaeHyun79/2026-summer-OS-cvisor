# 이 프로젝트의 C 문법 노트

`src/`(cvisor 본체), `ch6/`(OSTEP 측정 프로그램), `tests/`(추적 대상 예제)에
**실제로 등장하는** 문법과 키워드만 모았습니다. 조건문·반복문·`int`/`char` 같은
기본은 다루지 않습니다.

각 항목의 `파일:줄` 은 그 문법이 실제로 쓰인 위치입니다.

---

## 1. 선언 키워드

### `static` — 두 가지 완전히 다른 의미

같은 키워드가 **어디에 붙느냐**에 따라 뜻이 바뀝니다.

**(a) 파일 스코프에 붙으면 = "이 .c 파일 밖으로 내보내지 마라"**

```c
static int elf_analyze(trace_t *t, const char *path)          /* analyzer.c:24 */
static const struct { int64_t nr; const char *name; } SC_NAMES[] = { ... }
                                                              /* trace.c:40 */
```

링커에게 심볼을 숨깁니다. `analyzer.c`의 `is_all_digits`와 다른 파일의
`is_all_digits`가 이름이 같아도 충돌하지 않습니다. 이 프로젝트에서 `cvisor.h`에
선언된 함수(`analyze`, `record`, `tui_run` …)만 `static`이 없고, 나머지 내부
헬퍼는 전부 `static`입니다.

**(b) 함수 안의 지역 변수에 붙으면 = "정적 수명"**

이 프로젝트엔 없지만, 붙이면 그 변수는 스택이 아니라 `.data`/`.bss`에 놓여
함수를 나가도 값이 유지됩니다. (`tests/globals.c`가 다루는 그 `.bss`입니다.)

### `static inline`

```c
static inline uint64_t now_ns(void)   /* ch6/syscall_cost.c:37, ch6/ctx_switch.c:40 */
static inline uint64_t rdtsc(void)    /* ch6/syscall_cost.c:44 */
```

`inline`은 "호출 대신 본문을 그 자리에 펼쳐 달라"는 **힌트**(강제 아님).
측정 코드에서 중요한 이유는 함수 호출 오버헤드(call/ret, 스택 프레임)가
측정 구간에 섞이는 걸 막기 때문입니다.

C에서 `inline`을 단독으로 쓰면 링크 규칙이 까다로워서 실무에선 거의 항상
`static inline` 조합으로 씁니다.

### `extern`

```c
extern const regdesc_t CV_REGS[];   /* cvisor.h:122 */
extern const int       CV_NREGS;    /* cvisor.h:123 */
```

"이 변수는 **다른 곳에 실체가 있다**"는 선언. 실체는 `trace.c:10`, `trace.c:30`에
한 번만 정의되고, `cvisor.h`를 include한 모든 파일이 이 `extern` 선언을 통해
같은 배열을 참조합니다. 헤더에 실체를 두면 여러 .o에 중복 정의가 생겨
링크 에러가 납니다.

### `const` — 붙는 위치가 의미를 바꾼다

```c
const char *p;        /* p가 가리키는 곳이 상수. p 자체는 바꿀 수 있다 */
char * const p;       /* p 자체가 상수. 가리키는 내용은 바꿀 수 있다 */
const char * const p; /* 둘 다 상수 */
```

읽는 법: **`*`를 기준으로 오른쪽에서 왼쪽으로.**

프로젝트에서 자주 보이는 형태:

```c
void record_dump(const trace_t *t);              /* cvisor.h:136 — t를 읽기만 함 */
int record(..., char *const argv[], ...);        /* cvisor.h:134 */
const step_t *s = &t->steps[i];                  /* recorder.c:457 */
```

`char *const argv[]` = "포인터 배열인데, 각 포인터 자체는 못 바꾼다".
`execv()`의 시그니처를 그대로 따라간 것입니다.

`const trace_t *t`는 **문서 역할**이 큽니다 — 함수 시그니처만 보고 이 함수가
trace를 수정하지 않는다는 걸 알 수 있습니다. `analyze`/`record`는
`trace_t *t`(수정함), `analyze_dump`/`record_dump`는 `const trace_t *t`(출력만).

### `volatile`

```c
volatile long sink = 0; /* keep the loop honest */   /* ch6/syscall_cost.c:140 */
```

"컴파일러야, 이 변수는 네가 모르는 이유로 바뀔 수 있으니 최적화로 없애지 마라."

여기서의 목적은 하나입니다 — `for (i...) sink += read(fd, buf, 0);` 루프를
컴파일러가 "결과를 아무도 안 쓰네" 하고 통째로 지워버리는 걸 막는 것.
측정 대상 루프가 사라지면 0초가 나오겠죠.

원래 용도는 메모리 맵드 I/O 레지스터, 시그널 핸들러가 건드리는 변수 등입니다.
**멀티스레드 동기화 용도가 아닙니다** (그건 `_Atomic`/뮤텍스의 일).

### `typedef`

```c
typedef struct {
    uint64_t start, end;
} range_t;                /* cvisor.h:32-34 */
```

`struct { ... }`에 `range_t`라는 새 이름을 붙입니다. 이렇게 하면 매번
`struct range` 대신 `range_t`만 쓰면 됩니다. `_t` 접미사는 타입임을 나타내는
관례입니다 (`size_t`, `pid_t`, `uint64_t` 전부 같은 관례).

이 프로젝트의 모든 자료구조가 이 형태입니다:
`range_t`, `dline_t`, `insn_ref_t`, `lmap_t`, `heapreg_t`, `step_t`,
`scevent_t`, `outchunk_t`, `trace_t`, `regdesc_t`, `rec_ctx_t`,
`pending_sc_t`, `ui_t`.

### `enum`

```c
enum { PANE_STACK = 0, PANE_HEAP, PANE_GLOBALS, PANE_N };   /* tui.c:15 */

enum {
    CP_CUR = 1,   /* current source line / instruction */
    CP_CHG,       /* value changed vs previous step */
    ...
};                                                          /* tui.c:17-23 */
```

이름 붙인 정수 상수 묶음. 값을 안 쓰면 **앞 값 +1**로 자동 증가합니다.

두 가지 관용구가 보입니다:

- `PANE_N` — 마지막에 두어서 **개수**를 나타냄. `int mem_scroll[PANE_N];`
  (tui.c:30)처럼 배열 크기로 쓰고, `(u.mem_pane + 1) % PANE_N` (tui.c:705)으로
  순환시킵니다. enum에 항목을 추가해도 배열 크기와 순환이 자동으로 맞습니다.
- `CP_CUR = 1` — ncurses의 `init_pair()`가 1부터 시작하는 색 쌍 번호를
  요구하기 때문에 0을 건너뜁니다.

`#define`으로도 같은 걸 할 수 있지만, enum은 디버거가 이름을 알아보고
스코프 규칙을 따른다는 장점이 있습니다.

### 익명 구조체 배열

```c
static const struct { int64_t nr; const char *name; } SC_NAMES[] = {
    {0,"read"},{1,"write"},{2,"open"}, ...
};                                                          /* trace.c:40 */

static const struct { const char *name; int bit; } FLAG_BITS[] = {
    { "CF", 0 }, { "PF", 2 }, ...
};                                                          /* tui.c:172 */
```

타입 이름 없이 구조체를 정의하면서 동시에 배열을 만듭니다. 이 배열 하나에만
쓸 타입이라 이름을 지어줄 필요가 없을 때 쓰는 축약형입니다.

---

## 2. 고정폭 정수 타입 (`<stdint.h>`)

시스템 프로그래밍에서 `int`/`long`을 피하고 폭이 명시된 타입을 쓰는 이유는
**플랫폼마다 크기가 달라지기 때문**입니다. (`long`은 리눅스 64비트에서 8바이트,
윈도우에서 4바이트.)

| 타입 | 크기 | 범위 | 프로젝트에서의 용도 |
|---|---|---|---|
| `uint8_t` | 1바이트 | 0..255 | 메모리 스냅샷 바이트 (`step_t.stack`) |
| `int32_t` | 4바이트 | ±21억 | 소스 줄 번호, 인덱스 (`-1` = 없음) |
| `uint32_t` | 4바이트 | 0..42억 | `step_t.skipped` |
| `int64_t` | 8바이트 | ±922경 | syscall 번호·반환값 (음수 가능) |
| `uint64_t` | 8바이트 | 0..1844경 | **모든 주소와 레지스터 값** |

`uint64_t`가 지배적인 이유: x86-64의 포인터·레지스터가 정확히 64비트이고,
`0x7fffffffe000` 같은 주소를 `long`으로 다루면 이식성이 깨집니다.

`UINT64_MAX` (`syscall_cost.c:57`) — "아직 최솟값을 못 찾았다"를 나타내는
초기값 관용구.

### 크기/오프셋 전용 타입

| 타입 | 헤더 | 의미 |
|---|---|---|
| `size_t` | `<stddef.h>` | **부호 없는** 크기·개수·인덱스. `sizeof`의 결과 타입 |
| `ssize_t` | `<sys/types.h>` | 부호 **있는** 크기. `read`/`write`가 `-1`(에러)을 반환해야 해서 필요 |
| `off_t` | `<sys/types.h>` | 파일 오프셋 (`pread`의 인자) |
| `pid_t` | `<sys/types.h>` | 프로세스 ID |

```c
ssize_t n = read(c->out_fd, buf, sizeof(buf));   /* recorder.c:110 */
if (n <= 0) break;
```

`read`가 `ssize_t`인 것과 `size_t`가 부호 없다는 것이 결합해 흔한 버그를 만듭니다:

```c
size_t n = read(...);
if (n < 0)   /* 절대 참이 아님! size_t는 음수가 될 수 없다 */
```

`recorder.c:71-75`가 이걸 올바르게 처리합니다 — `ssize_t`로 받아 음수 검사를
먼저 하고, 그 다음에야 `(size_t)n`으로 캐스팅합니다.

### 부호 없는 뺄셈의 함정

`uint64_t`끼리 빼면 결과도 `uint64_t`라서, **작은 값 − 큰 값 = 거대한 양수**가
됩니다. `now_ns()` 기반 코드가 `CLOCK_MONOTONIC`(뒤로 가지 않는 시계)을
고른 게 이 함정을 피하기 위한 전제입니다.

### 정수 리터럴 접미사

```c
(uint64_t)ts.tv_sec * 1000000000ull   /* syscall_cost.c:41 */
uret < 0xfffffffffffff000ULL          /* recorder.c:225 */
(p->args[1] + 4095) & ~4095ULL        /* recorder.c:228 */
(16u << 20)                           /* cvisor.h:28 */
```

- `u` / `U` — unsigned
- `l` / `L` — long
- `ull` / `ULL` — unsigned long long (64비트 보장)

**왜 필요한가**: 접미사가 없으면 리터럴은 기본 `int`(32비트)로 취급됩니다.
`ts.tv_sec * 1000000000`에서 왼쪽을 `uint64_t`로 캐스팅했으니 여기선 안전하지만,
`16 << 20`처럼 int끼리 계산하면 결과도 int라 32비트 범위를 넘으면 오버플로합니다.
`~4095` 역시 int로 계산되면 상위 32비트가 0으로 남아 마스킹이 깨집니다 —
그래서 `~4095ULL`.

### 명시적 캐스팅

```c
(unsigned long long)s->regs.rip     /* recorder.c:461 — printf %llx 때문 */
(int32_t)(t->n_dlines - 1)          /* analyzer.c:212 — 좁은 타입에 저장 */
(unsigned char)*p                   /* analyzer.c:165 — isxdigit 요구사항 */
(void *)addr                        /* recorder.c:48 — 정수 주소 -> 포인터 */
(const char *)map + eh->e_shoff     /* analyzer.c:73 — 바이트 단위 산술 */
```

`isxdigit((unsigned char)*p)` 캐스팅은 진짜 버그 방지책입니다. `<ctype.h>`
함수들은 인자가 `unsigned char` 범위이거나 `EOF`여야 하는데, `char`가 부호
있는 플랫폼(x86 리눅스)에서 0x80 이상 바이트가 음수로 전달되면 **정의되지 않은
동작**입니다.

---

## 3. 포인터

### 기본 연산자

```c
&x        /* x의 주소 */
*p        /* p가 가리키는 값 (역참조) */
p->field  /* (*p).field 의 축약 */
p.field   /* p가 포인터가 아닐 때 */
```

이 프로젝트는 대부분 구조체를 포인터로 넘기므로 `->`가 압도적으로 많습니다.
`main.c`에서만 `trace_t t;`를 값으로 잡고 `t.n_steps`(main.c:86)처럼 `.`를 씁니다.

### `void *` — 타입 없는 포인터

```c
void *map = mmap(NULL, ...);                    /* analyzer.c:37 */
const Elf64_Ehdr *eh = map;                     /* analyzer.c:44 — 자동 변환 */
static ssize_t read_mem(..., void *buf, ...)    /* recorder.c:44 */
```

"어떤 타입인지는 모르지만 메모리 주소"라는 뜻. C에서는 `void *`와 다른 객체
포인터 간 변환에 **캐스팅이 필요 없습니다** (C++와 다른 점). `read_mem`이
`void *buf`를 받으므로 호출자는 `uint8_t*`든 `char*`든 그냥 넘길 수 있습니다.

`void *`는 역참조할 수 없고 산술도 불가합니다. 그래서 analyzer.c:73은
`(const char *)map + eh->e_shoff`처럼 **1바이트 타입으로 캐스팅한 뒤** 오프셋을
더합니다. 이게 바이너리 포맷 파싱의 표준 관용구입니다.

### 이중 포인터 `**`

두 가지 용도가 있고, 프로젝트에 둘 다 있습니다.

**(a) 문자열 배열**

```c
char **src;  int n_src;      /* cvisor.h:94 — 소스 파일의 각 줄 */
int main(int argc, char **argv)
```

`char *` 하나가 문자열 하나, 그게 여러 개 = `char **`.

**(b) 출력 매개변수(out-parameter)**

```c
static void mem_region(ui_t *u, uint64_t *base, size_t *len,
                       const uint8_t **buf, const char **name)   /* tui.c:231 */
{
    *base = s->stack_base; *len = s->stack_len; *buf = s->stack;
    *name = "stack";
}
```

C 함수는 값을 하나만 반환합니다. 여러 개를 돌려주려면 **호출자가 변수를 만들고
그 주소를 넘겨** 함수가 채우게 합니다. 호출부(tui.c:322-323):

```c
uint64_t base; size_t len; const uint8_t *buf; const char *name;
mem_region(u, &base, &len, &buf, &name);
```

`buf`가 이미 `const uint8_t *`이므로, 그 주소는 `const uint8_t **`가 됩니다.

같은 패턴이 `step_byte(..., uint8_t *out)` (tui.c:46)에도 있습니다 — 반환값은
성공/실패(0/-1), 실제 데이터는 `*out`으로.

### 포인터 산술

```c
(const char *)r + CV_REGS[i].off      /* trace.c:35 — 구조체 안 임의 오프셋 */
t->prog_output + t->out_len           /* trace.c:126 — 버퍼 끝에 이어쓰기 */
buf + n                               /* recorder.c:75 */
shstr + sh[i].sh_name                 /* analyzer.c:78 — 문자열 테이블 인덱싱 */
(size_t)(gt + 1 - lt)                 /* analyzer.c:172 — 포인터끼리 빼서 길이 */
```

포인터 + 정수 = **타입 크기 단위로** 이동합니다. `uint64_t *p`에서 `p + 1`은
8바이트 뒤입니다. 그래서 바이트 단위 계산에는 `char *`/`uint8_t *`를 씁니다.

포인터 − 포인터 = 그 사이의 **원소 개수** (`ptrdiff_t` 타입). analyzer.c:172의
`gt + 1 - lt`가 `"<main>:"` 부분 문자열의 길이를 구하는 방식입니다.

### 배열과 포인터

```c
uint64_t args[6];                  /* cvisor.h:79 — 구조체 멤버는 진짜 배열 */
static void f(uint64_t args[6])    /* 매개변수에서는 uint64_t* 와 동일 */
```

**함수 매개변수의 배열 표기는 포인터로 붕괴(decay)합니다.** `[6]`은 문서일 뿐
컴파일러가 크기를 검사하지 않습니다. 그래서 `sizeof(args)`가 함수 안에서는
48이 아니라 8(포인터 크기)이 됩니다.

반면 구조체 멤버 `uint64_t args[6]`은 진짜 48바이트이고, 그래서
`memcpy(e->args, p->args, sizeof(e->args))` (recorder.c:212)가 올바르게 48을
복사합니다.

### 함수 포인터

명시적으로 선언하진 않지만 `qsort`에 넘길 때 등장합니다:

```c
static int lmap_cmp(const void *a, const void *b)   /* analyzer.c:231 */
{
    const lmap_t *x = a, *y = b;
    if (x->addr < y->addr) return -1;
    if (x->addr > y->addr) return 1;
    return 0;
}
...
qsort(t->lmap, t->n_lmap, sizeof(lmap_t), lmap_cmp);  /* analyzer.c:395 */
```

**함수 이름은 그 자체로 함수 포인터**입니다 (`&lmap_cmp`와 `lmap_cmp`가 동일).
`qsort`는 어떤 타입을 정렬하는지 모르므로 비교 함수가 `const void *`를 받고,
안에서 실제 타입으로 되돌립니다.

비교 함수 규약: `a < b`면 음수, 같으면 0, `a > b`면 양수.

> `return x->addr - y->addr;`로 줄여 쓰면 안 됩니다 — `uint64_t` 뺄셈이
> 오버플로하고, `int`로 잘리면서 부호가 뒤집힐 수 있습니다. analyzer.c가
> 세 갈래로 나눠 쓴 이유입니다.

---

## 4. 구조체 관용구

### 지정 초기화자 (designated initializer, C99)

```c
rec_ctx_t c = {
    .pid = pid, .mem_fd = -1, .use_pvr = 1, .out_fd = pfd[0],
};                                              /* recorder.c:281 */

ui_t u = { .t = t, .cur = 0, .stack_desc = 1 };  /* tui.c:686 */
```

멤버 이름으로 초기화합니다. 장점:

- **언급하지 않은 멤버는 자동으로 0**으로 초기화됩니다. `rec_ctx_t`의
  `stack_top`, `brk_heap`, `anonr`, `n_anonr`는 전부 0이 됩니다.
- 구조체 멤버 **순서가 바뀌어도** 코드가 안 깨집니다.
- `.mem_fd = -1`처럼 0이 아닌 기본값을 쓰는 멤버만 눈에 띄어 의도가 드러납니다.

### 복합 리터럴 (compound literal, C99)

```c
t->chunks[t->n_chunks++] = (outchunk_t){ step_idx, off, (size_t)n };  /* recorder.c:123 */
c->anonr[c->n_anonr++]   = (range_t){ uret, uret + len };             /* recorder.c:229 */
```

`(타입){ 초기값 }` — **이름 없는 구조체 값을 그 자리에서** 만듭니다.
임시 변수 하나를 아낍니다:

```c
/* 복합 리터럴 없이 쓰면 */
outchunk_t tmp;
tmp.step = step_idx; tmp.off = off; tmp.len = (size_t)n;
t->chunks[t->n_chunks++] = tmp;
```

### 구조체 통째 대입

```c
s->regs = *regs;              /* recorder.c:135 */
t->globals_rng = data;        /* analyzer.c:100 */
c->anonr[i] = c->anonr[--c->n_anonr];   /* recorder.c:237 */
```

C는 구조체 대입을 **전체 바이트 복사**로 처리합니다 (`memcpy`와 동등).
`struct user_regs_struct`(약 216바이트)가 한 줄로 복사됩니다.

> 단, 배열은 대입할 수 없습니다. `int a[5], b[5]; a = b;`는 컴파일 에러 —
> `memcpy`를 써야 합니다. 구조체만 예외입니다.

recorder.c:237의 `c->anonr[i] = c->anonr[--c->n_anonr];`는 "순서 무관 배열에서
원소 삭제" 관용구입니다 — 마지막 원소를 삭제할 자리로 옮기고 개수를 줄입니다.
O(n) 이동이 필요 없어 O(1)입니다.

### `offsetof`

```c
#include <stddef.h>
{ "RIP", offsetof(struct user_regs_struct, rip) },   /* trace.c:11 */
```

구조체 시작점부터 특정 멤버까지의 **바이트 오프셋**을 컴파일 타임에 구합니다.

이걸 쓰는 이유가 `trace.c:32-37`에 있습니다:

```c
uint64_t cv_reg(const struct user_regs_struct *r, int i)
{
    uint64_t v;
    memcpy(&v, (const char *)r + CV_REGS[i].off, sizeof(v));
    return v;
}
```

레지스터 18개를 `r->rip`, `r->rsp`, `r->rbp` … 18갈래 `switch`로 꺼내는 대신,
**이름과 오프셋 테이블**을 만들어 `i`번째 레지스터를 루프로 처리합니다.
`record_dump`(recorder.c:467)와 `draw_reg`(tui.c:192)가 이 테이블 하나를
공유하므로, 레지스터를 추가하려면 `CV_REGS[]`에 한 줄만 넣으면 됩니다.

### `sizeof` 배열 길이 관용구

```c
const int CV_NREGS = (int)(sizeof(CV_REGS) / sizeof(CV_REGS[0]));   /* trace.c:30 */
#define N_FLAG_BITS ((int)(sizeof(FLAG_BITS) / sizeof(FLAG_BITS[0])))  /* tui.c:176 */

for (size_t i = 0; i < sizeof(SC_NAMES) / sizeof(SC_NAMES[0]); i++)  /* trace.c:57 */
```

"전체 바이트 / 원소 하나의 바이트 = 원소 개수". 배열 리터럴에 항목을 추가하면
개수가 자동으로 따라옵니다.

**반드시 진짜 배열에만** 써야 합니다. 포인터에 쓰면 (포인터 크기 / 원소 크기)라는
무의미한 값이 나옵니다 — 위의 "배열과 포인터" 항목 참고.

### `sizeof(*p)` 관용구

```c
memset(s, 0, sizeof(*s));       /* trace.c:108 */
memset(t, 0, sizeof(*t));       /* trace.c:150 */
memset(&t, 0, sizeof(t));       /* main.c:60 */
```

`sizeof(step_t)` 대신 `sizeof(*s)`를 쓰면 `s`의 타입이 바뀌어도 자동으로
맞습니다. `sizeof`는 피연산자를 **실행하지 않으므로** `*s`가 NULL이어도 안전합니다
(컴파일 타임에 타입만 봄).

`realloc`에서도 같은 패턴: `realloc(t->steps, ncap * sizeof(step_t))` (trace.c:101).

---

## 5. 전처리기

### 인클루드 가드

```c
#ifndef CVISOR_H
#define CVISOR_H
...
#endif /* CVISOR_H */          /* cvisor.h:7-8, 150 */
```

같은 헤더가 두 번 include돼도 내용이 한 번만 처리되게 합니다. 없으면 구조체가
중복 정의되어 컴파일 에러가 납니다.

### 조건부 컴파일 + `#error`

```c
#if !defined(__linux__) || !defined(__x86_64__)
#error "cvisor targets Linux x86-64 only (see README.md, section 2)"
#endif                          /* cvisor.h:10-12 */
```

`__linux__`, `__x86_64__`는 컴파일러가 자동으로 정의하는 **미리 정의된 매크로**입니다.
arm64 Mac에서 실수로 빌드하면 링크 에러나 런타임 크래시 대신 이 명확한 메시지를
받습니다. 이 프로젝트가 QEMU 안의 x86-64 리눅스에서만 도는 이유가 여기 명시돼
있습니다.

### 상수 매크로

```c
#define CV_DEFAULT_MAX_STEPS 200000
#define CV_STACK_RED_ZONE    64
#define CV_MMAP_TRACK_MAX    (16u << 20)    /* cvisor.h:20-28 */
#define RES_SAMPLES  10000                  /* syscall_cost.c:34 */
```

전처리기가 **텍스트 그대로 치환**합니다. `(16u << 20)`의 괄호가 중요한 이유:

```c
#define X 16u << 20
... X * 2      /* -> 16u << 20 * 2  = 16 << 40  (연산자 우선순위 사고!) */
```

**매크로 본문은 항상 괄호로 감싸는 것**이 규칙입니다. `N_FLAG_BITS`(tui.c:176)가
바깥 괄호를 한 겹 더 두른 것도 같은 이유입니다.

### `_GNU_SOURCE` — 기능 테스트 매크로

```c
#define _GNU_SOURCE
#include <fcntl.h>      /* 반드시 이 순서 */
```

glibc/musl 헤더가 어떤 API를 노출할지 정하는 스위치입니다. 헤더 안이 이렇게
생겼습니다:

```c
#ifdef _GNU_SOURCE
ssize_t process_vm_readv(...);
#endif
```

| 매크로 | 노출 범위 |
|---|---|
| `_POSIX_C_SOURCE=200809L` | POSIX.1-2008 |
| `_DEFAULT_SOURCE` | POSIX + BSD/SVID (기본값) |
| `_GNU_SOURCE` | 위 전부 + GNU/리눅스 확장 |

**반드시 첫 `#include`보다 앞에** 와야 합니다 — 헤더가 이미 파싱된 뒤엔
효과가 없습니다. 컴파일 옵션 `-D_GNU_SOURCE`도 동등합니다.

이 프로젝트에서 실제로 필요한 곳은 `recorder.c`입니다 — `<sys/uio.h>`의
`process_vm_readv` 선언이 `_GNU_SOURCE`로 가려져 있습니다. 나머지 파일
(`analyzer.c`, `tui.c`, `main.c`, `ch6/*.c`)은 순수 POSIX API만 쓰므로 관례적으로
붙인 것이고, 빼도 지금은 그대로 빌드됩니다.

### 문자열 리터럴 자동 연결

```c
fprintf(stderr,
    "cvisor: %s is a PIE binary. Rebuild with:\n"
    "  gcc -g -O0 -no-pie -fno-omit-frame-pointer -o target target.c\n"
    "(PIE support is a Phase 3 item; ...)\n", path);    /* analyzer.c:56-60 */
```

인접한 문자열 리터럴은 컴파일 타임에 **하나로 합쳐집니다**. `+`도 `\` 줄바꿈도
필요 없습니다. 긴 메시지를 80칼럼 안에 넣는 표준 방식입니다.

---

## 6. 기본 제어문 밖의 흐름 제어

### `goto` — 에러 처리 전용

```c
if (eh->e_machine != EM_X86_64) {
    fprintf(stderr, "...");
    goto out;
}
...
t->entry = eh->e_entry;
rc = 0;
out:
    munmap(map, (size_t)st.st_size);
    return rc;                          /* analyzer.c:44-111 */
```

"goto는 나쁘다"의 유일한 예외로 널리 인정되는 패턴입니다. **정리 코드를 한 곳에
모으기** 위한 용도입니다. 위 함수에는 실패 지점이 5군데인데, 각각에서
`munmap`을 반복하는 대신 `goto out` 하나로 모읍니다.

`rc` 변수의 역할에 주목하세요 — `-1`로 시작해서 성공 경로에서만 `0`이 됩니다.
모든 `goto out`은 `rc = -1`인 상태로 뛰어들므로 실패가 자동으로 전파됩니다.

`recorder.c`는 정리 단계가 여러 종류라 레이블도 여럿입니다:

| 레이블 | 의미 |
|---|---|
| `kill_out_ok` (:433) | 대상 프로세스를 죽이지만 **기록은 유효** → `0` 반환 |
| `kill_out` (:442) | 진짜 실패 → `-1` 반환 |

**규칙**: `goto`는 아래쪽으로만, 같은 함수 안에서만, 정리 목적으로만.
위로 뛰어 루프를 만들면 그때부터 읽을 수 없는 코드가 됩니다.

recorder.c:318의 `goto sc_check;`는 예외적으로 앞으로 뛰어 루프 본문의 일부를
건너뜁니다 — "이 명령은 이미 스냅샷했으니 재기록하지 말고 syscall 검사부터"라는
뜻이고, 주석으로 이유를 밝혀뒀습니다.

### `switch`의 세부 문법

```c
switch (p->nr) {
case 12:                       /* brk */
    maps_find(...);
    break;

case 9: {                      /* mmap — 블록 안에 지역 변수 선언 */
    uint64_t uret = (uint64_t)ret;
    int anon = ...;
    break;
}

case 56: case 57: case 58: case 59: case 435:   /* 여러 라벨이 한 코드로 */
    if (t->fork_step < 0 && recording)
        ...
    break;
}                                               /* recorder.c:216-247 */
```

세 가지 문법 포인트:

1. **`case`에 중괄호 블록** — `switch` 본문 전체가 하나의 스코프라서, 중괄호
   없이 `case` 안에서 변수를 선언하면 "다른 case가 이 변수를 건너뛰어 초기화
   없이 접근 가능"해져 컴파일러가 경고/에러를 냅니다. `{ }`로 감싸면 해결됩니다.

2. **여러 `case` 라벨 나열** — `case 56: case 57:`는 fallthrough(폴스루)를
   의도적으로 쓴 것이고, 이 형태는 경고 대상이 아닙니다. clone/fork/vfork/execve/clone3
   전부 같은 처리를 합니다.

3. **`break` 누락 = fallthrough** — 실수의 대표적 원인이라, 의도적일 땐 보통
   `/* fallthrough */` 주석을 답니다. 이 프로젝트엔 의도적 폴스루가 없습니다.

같은 패턴이 tui.c:692의 키 디스패치에도 있습니다:
```c
case KEY_RIGHT: case 'n':      /* 방향키와 'n' 둘 다 */
```

`default: continue;` (tui.c:743-744)는 영리한 부분입니다 — 모르는 키는
`redraw(&u)`를 건너뛰고 곧장 다음 `getch()`로 갑니다.

### `for (;;)` — 무한 루프

```c
for (;;) { ... }   /* recorder.c:109, recorder.c:315, tui.c:638, tui.c:690 */
```

`while (1)`과 완전히 동일합니다. 일부 컴파일러가 `while(1)`에 "조건이 상수"
경고를 내던 역사 때문에 `for(;;)`가 관례로 굳었습니다. 탈출은 `break`/`goto`로.

### `do { } while` — 조건 없는 정렬 관용구

```c
if (line[i] == '\t')
    do { expanded[o++] = ' '; } while (o % 4);
else
    expanded[o++] = line[i];        /* analyzer.c:442-445 */
```

탭 하나를 **다음 4칸 경계까지** 공백으로 채웁니다. `do-while`인 이유:
이미 경계에 있을 때(`o % 4 == 0`)도 최소 한 칸은 넣어야 하기 때문입니다.
`while`이면 0칸이 되어 탭이 사라집니다.

### 증감 연산자를 식 안에서

```c
t->chunks[t->n_chunks++] = ...       /* recorder.c:123 — 넣고 나서 증가 */
step_t *s = &t->steps[t->n_steps++]; /* trace.c:107 */
d[--n] = '\0';                       /* analyzer.c:124 — 감소하고 나서 접근 */
buf[--n] = '\0';                     /* tui.c:650 */
max_steps = strtoull(argv[++i], ...) /* main.c:41 — 증가하고 나서 접근 */
c->anonr[--c->n_anonr]               /* recorder.c:237 */
```

- **후위** `x++` — 현재 값을 쓰고 나서 증가
- **전위** `++x` — 증가한 뒤 그 값을 씀

`main.c:41`의 `argv[++i]`는 `--max-steps N`에서 **N을 읽으면서 동시에 루프
인덱스를 하나 더 진행**시켜, 다음 반복에서 N이 옵션으로 재해석되는 걸 막습니다.

> 같은 변수를 한 식 안에서 두 번 이상 건드리면 정의되지 않은 동작입니다:
> `i = i++ + 1;` 같은 코드는 절대 쓰지 마세요. 위 예들은 전부 각 식에서
> 변수를 한 번씩만 수정합니다.

### `continue`를 이용한 조기 탈출

```c
while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "CU:", 3) == 0) { ... continue; }
    ...
    if (ntok < 3)     continue;
    if (addr_i < 2)   continue;
    if (addr == 0)    continue;
    /* 여기까지 왔으면 유효한 행 */
}                                     /* analyzer.c:275-353 */
```

중첩 `if`를 쌓는 대신 "해당 없음 → 다음 줄"로 빠르게 나갑니다. 파서에서
들여쓰기 깊이를 낮게 유지하는 표준 기법입니다.

### 세미콜론만 있는 루프 본문

```c
while (now_ns() - t0 < 200000000ull) /* busy-wait ~200 ms */
    ;                                /* syscall_cost.c:117-118 */
```

본문이 비어 있음을 나타냅니다. **세미콜론을 다음 줄에 따로 두는 것**이
관례입니다 — `while (...);`처럼 붙여 쓰면 실수로 보이기 때문입니다.

### 삼항 연산자 중첩

```c
u->mem_pane == PANE_STACK ? "stack" :
u->mem_pane == PANE_HEAP  ? "heap"  : "globals"     /* tui.c:508-509 */
```

`조건 ? A : B`. 오른쪽 결합이라 위 코드는
`cond1 ? "stack" : (cond2 ? "heap" : "globals")`로 해석됩니다.
`printf` 인자 자리처럼 문장을 쓸 수 없는 곳에서 유용합니다.

간단한 형태들:
```c
size_t ncap = t->cap_steps ? t->cap_steps * 2 : 1024;   /* trace.c:100 */
r == 0 ? "" : "mmap:"                                   /* recorder.c:502 */
is_cur ? '>' : ' '                                      /* tui.c:122 */
```

---

## 7. 비트 연산

시스템 프로그래밍에서 없어서는 안 되는 부분입니다.

| 연산자 | 뜻 |
|---|---|
| `&` | AND — 비트 추출/마스킹 |
| `\|` | OR — 비트 설정 |
| `^` | XOR |
| `~` | NOT (비트 반전) |
| `<<` `>>` | 왼쪽/오른쪽 시프트 |

### 아래로 정렬 (align down)

```c
uint64_t lo = regs->rsp - CV_STACK_RED_ZONE;
lo &= ~(uint64_t)7;                    /* recorder.c:138-139 */
uint64_t lo = base & ~(uint64_t)7;     /* tui.c:339 */
```

`7` = `0b111`, `~7` = `...11111000`. AND하면 하위 3비트가 0이 되어 **8의 배수로
내림**합니다. 16진 덤프를 8바이트 단위로 정렬하기 위한 것입니다.

### 위로 정렬 (align up)

```c
uint64_t hi = (base + len + 7) & ~(uint64_t)7;         /* tui.c:340 */
uint64_t len = (p->args[1] + 4095) & ~4095ULL;         /* recorder.c:228 */
reg_rows[i] = 1 + (int)((s->heapr[i].len + 7) / 8);    /* tui.c:286 */
```

**`(x + (N-1)) & ~(N-1)`** = N의 배수로 올림 (N이 2의 거듭제곱일 때).
recorder.c:228은 mmap 길이를 4096바이트 페이지 경계로 올립니다 — 커널이
실제로 그렇게 할당하기 때문입니다.

나눗셈 버전 `(len + 7) / 8`은 "필요한 행 수"를 구하는 **올림 나눗셈**입니다.

### 플래그 비트 검사

```c
int anon     = (p->args[3] & 0x20) != 0;  /* MAP_ANONYMOUS */
int writable = (p->args[2] & 0x2)  != 0;  /* PROT_WRITE */   /* recorder.c:223-224 */
```

mmap의 flags/prot 인자에서 특정 비트만 뽑아냅니다. `!= 0`을 붙이는 이유는
결과를 0/1로 정규화하기 위해서입니다 (`args[3] & 0x20`은 0 아니면 0x20).

### 비트 추출 + 시프트

```c
int set = (fl >> FLAG_BITS[i].bit) & 1;    /* tui.c:214 */
```

EFLAGS 레지스터의 n번째 비트를 최하위로 내린 뒤 1과 AND해서 0/1을 얻습니다.
`FLAG_BITS[]`(tui.c:172)가 CF=0, PF=2, ZF=6 … 비트 위치를 들고 있습니다.

### 시프트로 크기 표현

```c
#define CV_MMAP_TRACK_MAX (16u << 20)   /* cvisor.h:28 — 16 MiB */
char *big = malloc(1 << 20);            /* tests/bigheap.c — 1 MiB */
```

`1 << 20` = 2²⁰ = 1048576. `16 * 1024 * 1024`보다 짧고, "2의 거듭제곱"이라는
의도가 드러납니다.

### 64비트 값 조립

```c
return ((uint64_t)hi << 32) | lo;       /* syscall_cost.c:49 */
```

`rdtsc`는 결과를 EDX:EAX 두 32비트 레지스터로 나눠 돌려줍니다. 상위를 32비트
왼쪽으로 밀고 하위와 OR해서 64비트 하나로 합칩니다.

**캐스팅 위치가 중요합니다**: `(uint64_t)hi << 32`가 아니라 `hi << 32`로 쓰면
`hi`가 32비트 `unsigned`라 시프트 결과가 전부 날아갑니다.

---

## 8. `printf` 계열 포맷 지정자

타입이 안 맞으면 **컴파일러가 못 잡고 런타임에 쓰레기 값이 나오거나 크래시**합니다
(가변 인자 함수라 타입 정보가 없음). `-Wformat` 경고를 반드시 켜세요.

| 지정자 | 타입 | 프로젝트 예시 |
|---|---|---|
| `%zu` | `size_t` | `printf("step %zu", i)` recorder.c:460 |
| `%llx` | `unsigned long long` (16진) | `(unsigned long long)s->regs.rip` |
| `%lld` | `long long` | `(long long)e->nr` recorder.c:483 |
| `%u` | `unsigned int` | `s->skipped` recorder.c:464 |
| `%p` | 포인터 | `(void *)p` tests/heap.c |
| `%d` `%s` `%c` | int / 문자열 / 문자 | 곳곳 |
| `%x` | unsigned int (16진) | tests/globals.c |

### 폭·정밀도 수식어

```c
"%016llx"        /* 16자리, 빈 자리는 0으로 채움  — tui.c:203 */
"%-.*s"          /* 왼쪽 정렬 + 최대 길이를 인자로  — tui.c:483 */
"%.*s"           /* 최대 길이를 인자로            — tui.c:438 */
"%.255s"         /* 최대 255자                   — analyzer.c:391 */
"%5zu"           /* 최소 5칸 오른쪽 정렬          — tui.c:475 */
"%8.1f"          /* 최소 8칸, 소수점 1자리        — ch6/ctx_switch.c:140 */
"%c%4d  %-.*s"   /* 조합                        — tui.c:121 */
```

`*`는 **길이를 다음 인자에서 읽는다**는 뜻입니다:

```c
mvwprintw(u->wout, ..., "%.*s", (int)ll, ls);   /* tui.c:438 */
```

`ll`글자만큼만 출력합니다. 터미널 폭에 맞춰 문자열을 자를 때 필수적이고,
버퍼 오버런 없이 잘라내는 안전한 방법이기도 합니다. **`*`에 대응하는 인자는
반드시 `int`**여야 해서 `(int)` 캐스팅이 붙습니다.

### `snprintf` — 안전한 문자열 구성

```c
snprintf(path, sizeof(path), "/proc/%d/mem", c->pid);       /* recorder.c:59 */
snprintf(t->src_file, sizeof(t->src_file), "%s", s);        /* analyzer.c:285 */
```

**항상 `sizeof(버퍼)`를 두 번째 인자로** 넘깁니다. 넘치면 잘라내고 NUL을
붙여줍니다. `sprintf`(크기 인자 없음)와 `strcpy`는 버퍼 오버플로의 대표적
원인이므로 쓰지 마세요.

analyzer.c:285의 `snprintf(dst, size, "%s", src)`는 `strncpy`의 안전한 대체제로도
쓰입니다 — `strncpy`는 버퍼가 꽉 차면 NUL을 안 붙이는 함정이 있습니다.

---

## 9. 표준 라이브러리 관용구

### 동적 배열: 2배 성장 (amortized growth)

이 프로젝트 전체에 반복되는 핵심 패턴입니다.

```c
step_t *trace_new_step(trace_t *t)
{
    if (t->n_steps == t->cap_steps) {
        size_t ncap = t->cap_steps ? t->cap_steps * 2 : 1024;
        step_t *ns = realloc(t->steps, ncap * sizeof(step_t));
        if (!ns)
            return NULL;              /* 실패 시 원본 t->steps는 그대로 유효 */
        t->steps = ns;
        t->cap_steps = ncap;
    }
    step_t *s = &t->steps[t->n_steps++];
    memset(s, 0, sizeof(*s));
    ...
}                                     /* trace.c:97-112 */
```

**`n_xxx`(현재 개수)와 `cap_xxx`(할당 용량)를 쌍으로 관리**하는 것이 핵심입니다.
`trace_t`에 `n_steps/cap_steps`, `out_len/out_cap`, `n_chunks/cap_chunks`,
`n_scs/cap_scs` 네 쌍이 있습니다.

**`realloc` 반환값을 절대 원래 변수에 바로 대입하면 안 됩니다:**

```c
t->steps = realloc(t->steps, ncap * sizeof(step_t));  /* 위험! */
```

실패하면 NULL이 들어오면서 **기존 포인터를 잃어버려 메모리 누수**입니다.
임시 변수(`ns`)로 받아 검사한 뒤 대입하는 것이 정석입니다.

같은 패턴의 다른 인스턴스: `trace_append_output`(trace.c:114),
`drain_output`(recorder.c:115), `handle_syscall`(recorder.c:199),
`add_dline`(analyzer.c:131), `parse_disasm`(analyzer.c:204),
`load_source`(analyzer.c:448).

> `analyzer.c:131`의 `if (t->n_dlines % 1024 == 0)`는 변형입니다 —
> 2배가 아니라 1024개씩 선형 증가.

### `malloc` / `free` / 누수 방지

`trace_free`(trace.c:130-151)가 해제 순서의 교과서적 예시입니다:

```c
for (size_t i = 0; i < t->n_dlines; i++)
    free(t->dlines[i].text);        /* 안쪽 먼저 */
free(t->dlines);                    /* 바깥은 나중 */
```

**중첩 할당은 안에서 밖으로** 해제합니다. 반대로 하면 이미 해제된 배열을
순회하게 됩니다.

`free(NULL)`은 **아무 일도 안 하며 안전**합니다. 그래서 `if (p) free(p);`는
불필요합니다.

마지막 `memset(t, 0, sizeof(*t))` (trace.c:150)는 해제 후 남은 포인터를
전부 NULL로 만들어 use-after-free를 방지합니다.

### `mem*` 함수군 (`<string.h>`)

| 함수 | 용도 | 프로젝트 예시 |
|---|---|---|
| `memcpy(d, s, n)` | 복사 (영역이 **겹치면 안 됨**) | trace.c:35, recorder.c:212 |
| `memmove(d, s, n)` | 복사 (겹쳐도 안전, 조금 느림) | tui.c:414 |
| `memset(p, c, n)` | n바이트를 c로 채움 | trace.c:108 |
| `memcmp(a, b, n)` | n바이트 비교 | analyzer.c:46 (ELF 매직) |
| `memchr(p, c, n)` | n바이트 안에서 c 찾기 | tui.c:430 |

`memcpy` vs `memmove`의 차이가 tui.c:414에서 실제로 중요합니다:

```c
memmove((void *)lines, (void *)(lines + 1), 511 * sizeof(char *));
```

배열을 자기 자신 위로 한 칸 밀기 때문에 **원본과 대상이 겹칩니다**.
`memcpy`를 쓰면 정의되지 않은 동작입니다.

trace.c:35의 `memcpy(&v, ptr, sizeof(v))`는 **정렬되지 않았을 수 있는 메모리에서
안전하게 읽는** 관용구입니다. `*(uint64_t *)ptr`로 직접 캐스팅해 읽으면
정렬이 안 맞을 때 아키텍처에 따라 크래시하거나 UB입니다. 컴파일러는 이 `memcpy`를
단일 load 명령으로 최적화하므로 성능 손해도 없습니다.

### `str*` 함수군

| 함수 | 용도 | 예시 |
|---|---|---|
| `strcmp(a, b)` | 전체 비교, 같으면 **0** | main.c:34 |
| `strncmp(a, b, n)` | 앞 n글자만 비교 | analyzer.c:179, tui.c:627 |
| `strlen(s)` | 길이 (NUL 제외) | analyzer.c:122 |
| `strdup(s)` | malloc + 복사 (**free 필요**) | analyzer.c:120, 457 |
| `strchr(s, c)` | 문자 첫 위치, 없으면 NULL | analyzer.c:168 |
| `strstr(h, n)` | 부분 문자열 위치 | recorder.c:90 |
| `strerror(errno)` | errno → 사람이 읽는 문자열 | analyzer.c:28 |
| `strsignal(sig)` | 시그널 번호 → 이름 | recorder.c:516 |

**`strcmp`가 같을 때 0을 반환**한다는 점이 초보자를 자주 잡습니다:

```c
if (strcmp(argv[i], "--dump") == 0)    /* 올바름 */
if (strcmp(argv[i], "--dump"))         /* "다를 때" 참! */
```

`strncmp`의 실용적 용도가 tui.c:627에 있습니다:
```c
if (strncmp(x, "call", 4) == 0 || strncmp(x, "ret", 3) == 0)
```
`"call   0x401136 <add3>"` 전체를 비교할 순 없으니 **접두사만** 봅니다.

### 문자열 → 숫자 변환

```c
uint64_t addr = strtoull(p, &end, 16);          /* analyzer.c:167 — 16진, 끝 위치도 받음 */
max_steps = (size_t)strtoull(argv[++i], NULL, 10);  /* main.c:41 — 10진 */
int cpu = (argc > 1) ? atoi(argv[1]) : 0;       /* ch6/ctx_switch.c:67 */
long trips = (argc > 2) ? atol(argv[2]) : 50000;
```

`strtoull(문자열, &끝포인터, 진법)`:
- 세 번째 인자 `16` = 16진수 파싱 (`0x` 접두사 선택적)
- `0` = 접두사로 자동 판별 (`0x`→16진, `0`→8진, 그 외 10진)
- 두 번째 인자에 주소를 주면 **파싱이 멈춘 위치**를 알려줍니다

이 `end` 활용이 objdump 파서의 핵심입니다 (analyzer.c:190-194):

```c
uint64_t addr = strtoull(p, &end, 16);
if (*end != ':')       /* 주소 뒤에 콜론이 없으면 명령어 줄이 아님 */
    continue;
end++;                 /* 콜론 건너뛰고 */
while (*end == ' ' || *end == '\t')
    end++;             /* 공백 건너뛰면 명령어 텍스트 시작 */
```

`atoi`/`atol`은 짧지만 **에러를 구분할 수 없습니다** (실패도 0, `"0"`도 0).
견고성이 필요하면 `strtoull`을 쓰세요.

### `strtok_r` — 재진입 가능한 토큰 분리

```c
char *tok[64];
int ntok = 0;
char *save = NULL;
for (char *p = strtok_r(line, " \t\n", &save);
     p && ntok < 64;
     p = strtok_r(NULL, " \t\n", &save))
    tok[ntok++] = p;                        /* analyzer.c:289-294 */
```

문자열을 구분자로 쪼갭니다. 세 가지 특징:

1. **첫 호출은 문자열, 이후는 `NULL`** — 같은 문자열을 계속 처리한다는 표시.
2. **원본을 파괴합니다** — 구분자 자리에 `\0`을 심습니다. 그래서 `line`은
   반드시 수정 가능한 배열이어야 하고(문자열 리터럴 불가), 이후에 원본을
   다시 쓸 수 없습니다.
3. `_r`(reentrant) 버전은 진행 상태를 `save`에 저장합니다. `strtok`(비-`_r`)은
   전역 상태를 써서 중첩 사용이나 멀티스레드에서 깨집니다. **항상 `_r`을 쓰세요.**

`tok[]` 배열에 포인터만 담고 복사하지 않는 것도 포인트 — 모두 `line` 내부를
가리킵니다.

### `qsort` — 표준 정렬

```c
qsort(t->lmap, t->n_lmap, sizeof(lmap_t), lmap_cmp);   /* analyzer.c:395 */
```

`qsort(배열, 원소수, 원소크기, 비교함수)`. 타입에 무관하려고 크기를 명시적으로
받고 비교 함수가 `const void *`를 씁니다. 비교 함수는 위 "함수 포인터" 항목 참고.

### 파일 입출력 두 계층

이 프로젝트는 **두 가지 I/O API를 목적에 따라 나눠 씁니다.**

**(a) 버퍼링되는 stdio (`FILE *`) — 텍스트 파싱용**

```c
FILE *fp = fopen(path, "r");
char line[512];
while (fgets(line, sizeof(line), fp)) { ... }
fclose(fp);                              /* recorder.c:84-100 */
```

`fgets(버퍼, 크기, 스트림)`은 한 줄씩 읽고 **개행 문자를 포함**합니다.
그래서 어디서나 뒤처리가 따라붙습니다:

```c
size_t n = strlen(line);
while (n && (line[n-1] == '\n' || line[n-1] == '\r'))
    line[--n] = '\0';                    /* analyzer.c:434-436 */
```

**(b) 원시 파일 디스크립터 (`int`) — 시스템 콜 직접**

```c
int fd = open(path, O_RDONLY);
pread(c->mem_fd, buf, len, (off_t)addr);   /* recorder.c:60-64 */
close(fd);
```

`/proc/<pid>/mem`처럼 임의 오프셋을 읽어야 하거나(`pread`), 파이프를
논블로킹으로 다뤄야 할 때(`fcntl(fd, F_SETFL, O_NONBLOCK)`, recorder.c:279)
씁니다.

**`pread` vs `read`**: `pread`는 오프셋을 인자로 받아 **파일 위치를 바꾸지
않습니다**. 매번 `lseek` + `read` 두 번 하는 것보다 낫고 원자적입니다.

### `popen` / `pclose` — 외부 명령의 출력 읽기

```c
snprintf(cmd, sizeof(cmd),
         "objdump -d --no-show-raw-insn '%s' 2>/dev/null", path);
FILE *fp = popen(cmd, "r");
while (fgets(line, sizeof(line), fp)) { ... }
pclose(fp);                              /* analyzer.c:151-215 */
```

셸 명령을 실행하고 그 stdout을 `FILE *`로 읽습니다. cvisor가 디스어셈블리와
DWARF 줄 정보를 **objdump에게 위임**하는 방식입니다.

`'%s'` 작은따옴표는 경로에 공백이 있어도 셸이 한 인자로 보게 합니다.
(다만 경로에 작은따옴표가 들어가면 여전히 깨지므로, 신뢰할 수 없는 입력에
`popen`을 쓰는 건 셸 인젝션 위험이 있습니다. 로컬 도구라 허용한 것입니다.)

**`fclose`가 아니라 `pclose`로 닫아야** 자식 프로세스를 회수합니다.
analyzer.c:342, 223에서 에러 경로에도 `pclose`가 빠짐없이 있는 걸 확인하세요.

### `errno` — 시스템 콜 에러 코드

```c
if (n >= 0)
    return n;
if (errno == ENOSYS || errno == EPERM)
    c->use_pvr = 0;      /* 이 커널엔 없는 기능 -> 대체 경로로 */
else
    return -1;                           /* recorder.c:51-55 */
```

시스템 콜이 `-1`을 반환했을 때 **왜** 실패했는지를 담는 전역 변수
(정확히는 스레드별 변수). `<errno.h>`.

**규칙**: 시스템 콜이 실패를 보고한 **직후에만** 읽으세요. 성공한 호출도
`errno`를 바꿀 수 있으므로 "실패 확인 → 즉시 errno 검사" 순서를 지켜야 합니다.

`perror("cvisor: fork")` (recorder.c:263)는 `"메시지: errno설명"`을 stderr에
출력하는 축약형이고, `strerror(errno)` (analyzer.c:28)는 문자열만 얻어
`fprintf`에 끼워 넣을 때 씁니다.

recorder.c:46-56은 **기능 탐지 후 폴백(fallback)** 패턴의 좋은 예입니다 —
빠른 `process_vm_readv`를 먼저 시도하고, 커널이 지원하지 않으면
`/proc/<pid>/mem`으로 내려갑니다.

---

## 10. 함수 시그니처 관례

### `(void)` 매개변수

```c
static inline uint64_t now_ns(void)    /* 인자 없음 */
int main(void)                         /* tests/*.c */
```

빈 괄호 `f()`는 C에서 "인자 정보를 밝히지 않음"(구식 K&R)이라 `f(1,2,3)`을
호출해도 컴파일러가 안 잡습니다. **`(void)`를 명시**해야 검사합니다.
(C23부터 빈 괄호도 `(void)`와 동일해졌지만 명시가 여전히 관례입니다.)

### 정수 반환 규약

이 프로젝트의 일관된 약속입니다:

```c
int  analyze(trace_t *t, const char *target_path);   /* 0 = 성공, -1 = 실패 */
int  record(...);                                    /* 같음 */
int32_t trace_insn_lookup(...);                      /* 인덱스, -1 = 못 찾음 */
step_t *trace_new_step(trace_t *t);                  /* 포인터, NULL = 실패 */
```

호출부(main.c:63)가 `if (analyze(&t, target) < 0) return 1;`처럼 일관되게
처리할 수 있습니다.

`-1`을 "없음"으로 쓰는 게 `step_t.src_line`, `insn_idx`, `trace_t.fork_step`,
`rec_ctx_t.mem_fd`에서도 반복됩니다. 그래서 이 필드들이 `uint32_t`가 아니라
**부호 있는** `int32_t`/`int64_t`입니다.

### `main`의 두 형태

```c
int main(void)                        /* 인자 안 씀 — tests/*.c */
int main(int argc, char **argv)       /* 인자 씀 — main.c, ch6/*.c */
```

`argv[0]`은 프로그램 이름, `argv[argc]`는 항상 `NULL`입니다.
반환값 0 = 성공, 0이 아니면 실패 (셸의 `$?`로 관찰).

main.c:44-53의 반환값 규약: `0`(도움말 출력 후 정상), `2`(사용법 오류),
`1`(실행 중 실패) — Unix 도구의 일반 관례입니다.

### `(void)변수;` — 미사용 경고 억제

```c
(void)sink;      /* syscall_cost.c:160, ctx_switch.c:61 */
(void)w;         /* tui.c:377 */
```

"이 변수를 안 쓰는 걸 알고 있다"고 컴파일러에게 알려 `-Wunused` 경고를
없앱니다. 아무 코드도 생성되지 않습니다.

---

## 11. 인라인 어셈블리 (`ch6/syscall_cost.c`만)

```c
__asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));   /* :48 */
```

GCC 확장 문법입니다. 형태:

```
__asm__ [__volatile__] ("명령어들" : 출력 : 입력 : clobber);
```

- `"lfence; rdtsc"` — 실행할 x86 명령
- `"=a"(lo)` — 출력 제약. `=`는 쓰기 전용, `a`는 **EAX 레지스터**를 뜻함.
  즉 "EAX의 값을 `lo` 변수에 넣어라"
- `"=d"(hi)` — 마찬가지로 **EDX**를 `hi`로
- `__volatile__` — "부작용이 있으니 최적화로 옮기거나 지우지 마라"

`rdtsc`가 결과를 EDX:EAX로 나눠 주는 하드웨어 규약을 그대로 반영한 것이고,
49행에서 `((uint64_t)hi << 32) | lo`로 다시 합칩니다.

`lfence`는 CPU의 비순차 실행(out-of-order)이 앞선 명령들을 `rdtsc` 뒤로
미루지 못하게 막는 직렬화 장벽입니다. 없으면 측정 구간이 샙니다.

**이 코드는 x86-64 전용**입니다 — arm64에는 `rdtsc`가 없습니다.

---

## 12. 헤더 정리

### `""` vs `<>`

```c
#include "cvisor.h"     /* 프로젝트 파일 — 소스와 같은 디렉터리부터 탐색 */
#include <stdio.h>      /* 시스템 헤더 — 표준 경로에서 탐색 */
```

### 이 프로젝트에서 쓰는 헤더

| 헤더 | 제공하는 것 |
|---|---|
| `<stdint.h>` | `uint64_t`, `int32_t`, `UINT64_MAX` |
| `<stddef.h>` | `size_t`, `NULL`, `offsetof` |
| `<stdio.h>` | `printf`, `FILE`, `fopen`, `fgets`, `snprintf`, `popen` |
| `<stdlib.h>` | `malloc`, `free`, `realloc`, `qsort`, `strtoull`, `exit` |
| `<string.h>` | `mem*`, `str*` |
| `<unistd.h>` | `read`, `write`, `close`, `fork`, `pipe`, `execv`, `_exit` |
| `<fcntl.h>` | `open`, `O_RDONLY`, `fcntl`, `O_NONBLOCK` |
| `<errno.h>` | `errno`, `ENOSYS`, `EPERM` |
| `<sys/ptrace.h>` | `ptrace`, `PTRACE_SINGLESTEP` 등 — **cvisor의 핵심** |
| `<sys/wait.h>` | `waitpid`, `WIFEXITED`, `WEXITSTATUS`, `WIFSTOPPED` |
| `<sys/user.h>` | `struct user_regs_struct` (x86-64 레지스터 전체) |
| `<sys/uio.h>` | `struct iovec`, `process_vm_readv` |
| `<sys/mman.h>` | `mmap`, `munmap`, `PROT_READ`, `MAP_PRIVATE` |
| `<sys/personality.h>` | `personality(ADDR_NO_RANDOMIZE)` — ASLR 끄기 |
| `<elf.h>` | `Elf64_Ehdr`, `Elf64_Shdr`, `ELFMAG`, `EM_X86_64` |
| `<signal.h>` | `SIGTRAP`, `SIGSEGV`, `kill` |
| `<ctype.h>` | `isdigit`, `isxdigit` |
| `<curses.h>` | ncurses TUI 전체 |
| `<libgen.h>` | `basename`, `dirname` |
| `<locale.h>` | `setlocale` |
| `<sched.h>` | `cpu_set_t`, `CPU_ZERO`, `CPU_SET`, `sched_setaffinity` |
| `<time.h>` | `clock_gettime`, `struct timespec`, `CLOCK_MONOTONIC` |
| `<sys/time.h>` | `gettimeofday`, `struct timeval` |

### `WIFEXITED` 계열 — 매크로로 위장한 비트 추출

```c
if (WIFEXITED(status))    t->exit_code    = WEXITSTATUS(status);
if (WIFSIGNALED(status))  t->death_signal = WTERMSIG(status);
if (WIFSTOPPED(status))   int sig         = WSTOPSIG(status);   /* recorder.c:375-384 */
```

`waitpid`가 채우는 `int status` 하나에 종료 방식·종료 코드·시그널 번호가
비트필드로 눌러 담겨 있습니다. 이 매크로들이 해당 비트를 꺼내줍니다.
직접 `status >> 8` 같은 걸 하면 안 됩니다 — 레이아웃은 구현 정의입니다.

---

## 13. 함께 읽으면 좋은 순서

문법을 익힌 뒤 실제 코드를 읽을 때 권하는 순서입니다.

1. **`tests/*.c`** — 15~20줄짜리. 추적 **대상**이라 문법이 가장 단순합니다.
2. **`src/cvisor.h`** — 자료구조 전체 지도. 여기만 이해하면 나머지가 쉽습니다.
3. **`src/main.c`** — 인자 파싱 + 세 단계(analyze → record → tui) 흐름.
4. **`src/trace.c`** — 동적 배열, 이진 탐색, offsetof 테이블. 가장 짧고 밀도 높음.
5. **`src/analyzer.c`** — 파일 I/O, 문자열 파싱, ELF 구조체, goto 에러 처리.
6. **`src/recorder.c`** — fork/exec/ptrace/파이프. OS 개념이 가장 많이 나옵니다.
7. **`src/tui.c`** — ncurses. 문법보다 라이브러리 API 비중이 큽니다.
8. **`ch6/*.c`** — 독립 실행 프로그램. 인라인 어셈블리와 측정 기법.

---

## 부록: 이 코드베이스에서 반복되는 실수 방지 패턴 요약

| 패턴 | 이유 |
|---|---|
| `realloc` 결과를 임시 변수로 받기 | 실패 시 원본 포인터 손실 방지 |
| `snprintf(buf, sizeof(buf), ...)` | 버퍼 오버플로 방지 |
| `ssize_t`로 받고 음수 검사 후 캐스팅 | `size_t`는 음수를 표현 못 함 |
| `isxdigit((unsigned char)*p)` | 음수 `char` 전달 시 UB |
| `memcpy(&v, ptr, sizeof(v))` | 정렬 안 된 읽기 회피 |
| `strtok_r` (`strtok` 아님) | 전역 상태 회피 |
| `_r`/`n`/`p` 붙은 변종 선호 | 재진입성·경계 검사·오프셋 안전 |
| `const trace_t *` 매개변수 | 수정하지 않음을 시그니처로 보증 |
| `goto cleanup` 단일 정리 지점 | 실패 경로마다 해제 코드 중복 제거 |
| `enum { ..., XXX_N }` | 배열 크기와 순환이 자동으로 동기화 |
