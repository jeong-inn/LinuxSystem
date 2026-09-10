# SSU-Cleanupd

SSU-Cleanupd는 Linux에서 지정한 디렉터리를 주기적으로 검사하고, 발견한 일반 파일을 확장자별 출력 디렉터리로 복사하는 대화형 프로그램이다. 사용자는 하나의 관리 셸에서 디렉터리별 정리 데몬을 등록하고, 실행 상태를 조회하고, 설정을 변경하거나 데몬을 종료할 수 있다.

이 문서는 별도 과제 명세를 읽지 않아도 현재 구현의 목적, 내부 동작, 명령 형식, 설정 파일, 빌드 방법과 테스트 절차를 이해할 수 있도록 작성되었다. 아래 설명은 현재 `ssu_cleanupd.c`의 실제 동작을 기준으로 한다.

## 1. 주요 특징

- `show`, `add`, `modify`, `remove`, `help`, `exit` 내장 명령을 제공한다.
- 하나의 모니터링 디렉터리마다 별도의 데몬 프로세스를 실행한다.
- 데몬은 모니터링 디렉터리 전체를 재귀적으로 탐색한다.
- 일반 파일을 확장자별 하위 디렉터리에 복사한다.
- 특정 하위 디렉터리 또는 특정 확장자를 정리 대상에서 제한할 수 있다.
- 같은 파일명이 여러 위치에 있을 때 mtime을 기준으로 최신 파일, 가장 오래된 파일 또는 아무 파일도 선택하지 않는 정책을 지원한다.
- 정리 성공 기록을 로그로 남기며, 로그의 최대 줄 수를 제한할 수 있다.
- 실행 중에도 config 파일을 통해 데몬 설정을 변경할 수 있다.
- config, log, 데몬 목록 파일에 `fcntl()` 기반 파일 잠금을 사용한다.
- 프로그램 전체에서 `system()`을 사용하지 않는다.

SSU-Cleanupd에서 “정리”는 원본 파일을 삭제하거나 이동하는 작업이 아니다. 원본은 그대로 두고 출력 디렉터리에 복사본을 만드는 방식이다.

## 2. 동작 구조

```text
사용자
  │
  ▼
ssu_cleanupd 관리 셸
  ├─ add ─────▶ 디렉터리별 데몬 생성
  ├─ show ────▶ 목록/config/log 조회
  ├─ modify ──▶ config 변경
  └─ remove ──▶ 데몬에 SIGTERM 전달
                    │
                    ▼
              설정 주기만큼 대기
                    │
                    ▼
            config 파일 잠금 및 재읽기
                    │
                    ▼
              디렉터리 재귀 탐색
                    │
                    ▼
        제외 경로·확장자·중복 정책 적용
                    │
                    ▼
          확장자 디렉터리로 파일 복사
                    │
                    ▼
                 로그 기록
```

관리 셸과 데몬의 수명은 분리되어 있다. `exit`로 관리 셸을 종료해도 등록된 데몬은 계속 실행된다. 데몬을 종료하려면 관리 셸을 다시 실행하고 `remove`를 사용해야 한다.

## 3. 요구 환경

- Linux 또는 POSIX 계열 환경
- C11을 지원하는 GCC 또는 Clang
- `make`
- 빌드 및 실행에 사용하는 사용자의 `HOME` 환경 변수

프로그램은 `realpath()`, `fork()`, `setsid()`, `fcntl()`, `sigaction()`, `nanosleep()` 등 POSIX API를 사용한다. 과제의 목표 환경은 Linux다.

## 4. 빌드

프로젝트 디렉터리에서 다음 명령을 실행한다.

```bash
make
```

기본 빌드 옵션은 다음과 같다.

```text
-std=c11 -Wall -Wextra -Wpedantic -O2
```

컴파일러를 명시하려면 다음과 같이 실행할 수 있다.

```bash
make CC=gcc
```

생성 파일을 삭제하려면 다음 명령을 사용한다.

```bash
make clean
```

`make clean`은 실행 파일 `ssu_cleanupd`와 오브젝트 파일 `ssu_cleanupd.o`만 삭제한다. 모니터링 디렉터리, 출력 파일, config, log 및 `$HOME/.ssu_cleanupd`는 삭제하지 않는다.

## 5. 실행

```bash
./ssu_cleanupd
```

정상 실행되면 다음 프롬프트가 나타난다.

```text
20211426>
```

실행 인자는 받지 않는다. 다음과 같이 인자를 추가하면 사용법을 출력하고 실패 상태로 종료한다.

```bash
./ssu_cleanupd anything
```

프로그램을 처음 실행하면 다음 상태 파일을 생성한다.

```text
$HOME/.ssu_cleanupd/
└── current_daemon_list
```

기존 `current_daemon_list`는 실행할 때 초기화하지 않는다. 목록에는 현재 구현의 내부 형식인 `PID,모니터링_절대경로`가 한 줄씩 기록된다.

```text
12345,/home/user/inbox
12346,/home/user/documents/source
```

`show`, `add`, `modify`, `remove`가 목록을 조회할 때 더 이상 존재하지 않는 PID는 목록에서 제거된다.

## 6. 경로 및 입력 규칙

모니터링 경로, 출력 경로, 제외 경로에는 상대경로와 절대경로를 사용할 수 있다. 프로그램은 입력 경로를 `realpath()`로 정규화한 뒤 절대경로로 저장한다.

경로에는 다음 규칙이 적용된다.

- 관련 디렉터리는 모두 현재 사용자의 `$HOME` 안에 있어야 한다.
- 심볼릭 링크가 입력되면 링크 자체가 아니라 해석된 실제 경로를 기준으로 HOME 범위를 검사한다.
- 모니터링 디렉터리는 존재해야 하며 읽기, 쓰기, 탐색 권한이 있어야 한다.
- `-d` 출력 디렉터리는 명령 실행 전에 존재해야 하며 읽기, 쓰기, 탐색 권한이 있어야 한다.
- `-x` 제외 디렉터리는 존재해야 하며 읽기와 탐색 권한이 있어야 한다.
- 출력 디렉터리와 모니터링 디렉터리는 서로 같거나 상·하위 관계일 수 없다.
- 이미 등록된 모니터링 경로와 같거나 그 경로의 상위 또는 하위인 경로는 추가 등록할 수 없다.
- 제외 경로는 모니터링 경로의 엄격한 하위 디렉터리여야 한다.
- 여러 제외 경로는 서로 같거나 상·하위 관계로 겹칠 수 없다.

공백이 있는 경로는 따옴표 또는 역슬래시로 입력할 수 있다.

```text
add "/home/user/my inbox" -i 5
add /home/user/my\ inbox -i 5
```

여기서 `/home/user`는 실제 홈 절대경로로 바꿔야 한다. SSU-Cleanupd 프롬프트는 Bash가 아니므로 `$HOME`, `~`, 와일드카드 같은 셸 표현을 확장하지 않는다. 프롬프트 안에서는 실제 절대경로를 입력하거나 프로그램을 실행한 디렉터리를 기준으로 한 상대경로를 입력해야 한다.

명령 한 줄은 최대 4,096바이트이며 최대 256개 토큰으로 분리한다. 내부 경로 버퍼는 4,096바이트, 파일명 버퍼는 255바이트를 기준으로 한다. 하나의 `-x` 또는 `-e` 설정에는 최대 256개 값을 저장한다. 실제 입력 가능한 개수는 전체 명령 길이 제한에 먼저 영향을 받을 수 있다.

빈 줄을 입력하면 아무 동작 없이 프롬프트를 다시 출력한다. 알 수 없는 명령을 입력하면 전체 도움말을 출력한다.

## 7. 내장 명령어

### 7.1 help

```text
help
```

사용할 수 있는 명령과 옵션의 간단한 사용법을 출력한다. 추가 인자를 허용하지 않는다.

### 7.2 exit

```text
exit
```

현재 관리 셸을 종료한다. 실행 중인 정리 데몬은 종료하지 않는다. 추가 인자를 허용하지 않는다.

### 7.3 add

```text
add <DIR_PATH> [OPTION]...
```

`DIR_PATH`를 재귀적으로 모니터링하는 데몬을 등록한다. 등록 시 모니터링 디렉터리에 `ssu_cleanupd.config`와 `ssu_cleanupd.log`를 생성하거나 연다.

옵션을 생략하면 다음 기본 설정을 사용한다.

| 옵션 | 설정 항목 | 입력 형식 | 기본값 | 용도 |
|---|---|---|---|---|
| `-d` | `output_path` | 기존 디렉터리 경로 1개 | `<DIR_PATH>_arranged` | 정리된 파일을 복사할 최상위 디렉터리 |
| `-i` | `time_interval` | 1 이상의 10진 정수 1개 | `10`초 | 디렉터리를 다시 검사할 시간 간격 |
| `-l` | `max_log_lines` | 1 이상의 10진 정수 1개 | `none` | log 파일에 유지할 최근 기록의 최대 줄 수 |
| `-x` | `exclude_path` | 하위 디렉터리 경로 1개 이상 | `none` | 재귀 탐색에서 완전히 제외할 디렉터리 |
| `-e` | `extension` | 확장자 1개 이상 | `all` | 복사 대상으로 인정할 파일 확장자 |
| `-m` | `mode` | `1`, `2`, `3` 중 하나 | `1` | 동일 파일명 충돌 처리 방식 |

모든 옵션은 한 명령에서 함께 사용할 수 있으며 순서는 자유롭다. 같은 옵션을 한 명령에서 두 번 사용하면 오류로 처리한다. 알 수 없는 옵션과 값이 없는 옵션도 오류로 처리한다.

#### `-d`: 출력 디렉터리

```text
add <DIR_PATH> -d <OUTPUT_PATH>
```

`OUTPUT_PATH`는 미리 존재하는 디렉터리여야 한다. `-d`를 생략하면 모니터링 경로의 마지막 이름 뒤에 `_arranged`를 붙인 형제 경로를 만들고 사용한다.

```text
모니터링: /home/user/data/inbox
기본 출력: /home/user/data/inbox_arranged
```

명시한 출력 경로와 모니터링 경로가 같거나 어느 한쪽이 다른 쪽을 포함하면 재귀 복사 위험을 막기 위해 등록을 거부한다.

#### `-i`: 검사 주기

```text
add <DIR_PATH> -i <TIME_INTERVAL>
```

초 단위 자연수를 입력한다. `0`, 음수, 실수, 숫자가 아닌 문자열은 허용하지 않는다. 데몬은 등록 직후 바로 검사하지 않고 현재 주기만큼 기다린 후 첫 검사를 수행한다.

#### `-l`: 로그 최대 줄 수

```text
add <DIR_PATH> -l <MAX_LOG_LINES>
```

로그에 보존할 최근 기록의 최대 개수다. 자연수만 허용한다. 설정하지 않으면 줄 수 제한 없이 기록한다. 제한이 설정되고 해당 정리 주기에 하나 이상의 새 로그가 기록되면, 주기 끝에 오래된 줄을 제거하고 최근 N줄만 남긴다. `modify`로 N을 줄였더라도 새로 복사되는 파일이 없다면 기존 로그는 즉시 잘리지 않고 다음 로그 기록 시점에 제한이 적용된다.

#### `-x`: 제외 디렉터리

다음 입력 형식을 모두 지원한다.

```text
add <DIR_PATH> -x <PATH1>,<PATH2>
add <DIR_PATH> -x <PATH1> <PATH2>
add <DIR_PATH> -x <PATH1>, <PATH2>
```

각 경로는 모니터링 경로 아래에 존재하는 디렉터리여야 한다. 제외된 디렉터리는 그 하위 전체를 열어보지 않는다. 여러 경로가 동일하거나 서로 상·하위로 겹치면 오류 처리한다.

#### `-e`: 확장자 필터

다음과 같이 쉼표 또는 공백으로 여러 확장자를 지정할 수 있다.

```text
add <DIR_PATH> -e txt,c,cpp
add <DIR_PATH> -e txt c cpp
```

확장자 앞의 점은 제거해서 저장한다. 따라서 `txt`와 `.txt`는 같은 값으로 처리한다. 확장자 비교는 대소문자를 구분하므로 `txt`와 `TXT`는 서로 다르다. 옵션을 생략하면 모든 확장자를 대상으로 한다.

확장자가 없는 파일, `.bashrc`처럼 점으로 시작하지만 추가 확장자가 없는 파일, 이름이 점으로 끝나는 파일은 `noext`로 분류한다. 이 파일만 선택하려면 `-e noext`를 사용할 수 있다.

#### `-m`: 동일 파일명 처리

“동일 파일명”은 확장자를 포함한 전체 basename이 같은 경우를 의미한다. 서로 다른 하위 디렉터리에 있는 파일도 모니터링 트리 전체에서 하나의 그룹으로 비교한다.

| 모드 | 동작 |
|---|---|
| `1` | 원본 후보 중 mtime이 가장 최신인 파일을 선택한다. 출력 파일이 이미 있으면 출력 파일보다 더 최신일 때만 덮어쓴다. |
| `2` | 원본 후보 중 mtime이 가장 오래된 파일을 선택한다. 출력 파일이 이미 있으면 출력 파일보다 더 오래됐을 때만 덮어쓴다. |
| `3` | 원본 후보가 둘 이상이거나 같은 이름의 출력 파일이 이미 있으면 아무 파일도 복사하지 않는다. 원본 후보가 하나이고 출력 파일이 없을 때만 복사한다. |

여러 원본의 mtime이 같으면 절대경로를 문자열로 비교했을 때 가장 앞선 파일을 선택한다. 출력 파일과 선택된 원본의 mtime이 같으면 모드 1과 2 모두 기존 출력 파일을 유지한다.

#### add 예시

다음 명령은 줄바꿈 없이 한 줄로 입력한다. `/home/user`는 실제 홈 절대경로로 바꾼다.

```text
add /home/user/cleanup-demo/inbox -d /home/user/cleanup-demo/arranged -i 5 -l 100 -x /home/user/cleanup-demo/inbox/cache,/home/user/cleanup-demo/inbox/tmp -e txt,c,cpp -m 1
```

관리 셸은 config 작성과 데몬 목록 등록을 완료한 후 프롬프트를 다시 출력한다.

### 7.4 show

```text
show
```

현재 살아 있는 데몬의 모니터링 절대경로를 번호와 함께 출력하고 상세 정보를 볼 번호를 입력받는다.

```text
Current working daemon process list

0. exit
1. /home/user/cleanup-demo/inbox

Select one to see process info : 1
```

유효한 번호를 선택하면 다음 내용을 출력한다.

1. 해당 디렉터리의 `ssu_cleanupd.config` 전문
2. 해당 디렉터리의 `ssu_cleanupd.log` 최근 10줄

로그가 10줄 미만이면 존재하는 모든 줄을 출력한다. `0`을 입력하면 `show`를 끝내고 기본 프롬프트로 돌아간다. 숫자가 아니거나 목록 범위를 벗어난 값을 입력하면 `Please check your input is valid`를 출력한 뒤 목록과 선택 프롬프트를 다시 보여준다.

`show`는 인자를 받지 않는다.

### 7.5 modify

```text
modify <DIR_PATH> <OPTION>...
```

현재 정확히 `DIR_PATH`를 모니터링하는 데몬의 설정을 변경한다. `add`와 동일한 `-d`, `-i`, `-l`, `-x`, `-e`, `-m` 옵션과 검증 규칙을 사용한다.

```text
modify /home/user/cleanup-demo/inbox -i 30 -l 20 -e c,cpp -m 2
```

입력한 옵션만 변경하며 다음 값은 보존한다.

- 입력하지 않은 옵션의 기존 설정
- 데몬 PID
- 최초 등록 시각
- 모니터링 경로

config 파일은 배타 잠금을 획득한 뒤 다시 작성한다. 데몬은 현재 진행 중인 대기 시간이 끝난 뒤 config를 다시 읽는다. 따라서 `-i`를 변경해도 이미 시작된 sleep의 길이는 즉시 바뀌지 않으며, 다음 주기부터 새 값이 사용된다.

현재 명령 인터페이스에는 기존 `-x`를 `none`으로 되돌리거나 기존 `-e`를 `all`로 되돌리는 전용 값이 없다. 해당 기본값으로 되돌려야 한다면 `remove` 후 필요한 기본 옵션으로 다시 `add`해야 한다. `modify`에는 최소 하나의 옵션이 필요하다.

### 7.6 remove

```text
remove <DIR_PATH>
```

정확히 `DIR_PATH`를 모니터링하는 데몬에 `SIGTERM`을 전달하고 `current_daemon_list`에서 해당 항목을 제거한다.

`remove`는 다음 파일과 디렉터리를 삭제하지 않는다.

- 모니터링 디렉터리와 원본 파일
- 출력 디렉터리와 복사된 파일
- `ssu_cleanupd.config`
- `ssu_cleanupd.log`

같은 경로를 다시 `add`하면 새 PID와 새 시작 시각으로 config를 다시 기록한다. `remove`는 경로 하나만 받으며 옵션이나 추가 인자를 허용하지 않는다.

## 8. config 파일

각 모니터링 디렉터리에 다음 파일이 존재한다.

```text
<DIR_PATH>/ssu_cleanupd.config
```

예시는 다음과 같다.

```text
monitoring_path : /home/user/cleanup-demo/inbox
pid : 12345
start_time : 2026-09-10 20:15:00
output_path : /home/user/cleanup-demo/arranged
time_interval : 5
max_log_lines : 100
exclude_path : /home/user/cleanup-demo/inbox/cache,/home/user/cleanup-demo/inbox/tmp
extension : txt,c,cpp
mode : 1
```

| 설정 키 | 의미 |
|---|---|
| `monitoring_path` | 재귀 탐색할 디렉터리의 절대경로 |
| `pid` | 이 디렉터리를 담당하는 데몬 프로세스 ID |
| `start_time` | `add`로 현재 데몬을 등록한 로컬 날짜와 시각 |
| `output_path` | 확장자별 디렉터리와 복사본이 저장되는 절대경로 |
| `time_interval` | 검사 간격(초) |
| `max_log_lines` | 최대 로그 줄 수 또는 제한이 없음을 나타내는 `none` |
| `exclude_path` | 쉼표로 구분한 제외 디렉터리 절대경로 또는 `none` |
| `extension` | 쉼표로 구분한 대상 확장자 또는 모든 확장자를 뜻하는 `all` |
| `mode` | 동일 파일명 처리 모드 1~3 |

데몬은 매 정리 주기마다 config를 다시 읽는다. 관리 셸의 `modify`와 데몬의 읽기가 겹치지 않도록 읽을 때 공유 잠금, 쓸 때 배타 잠금을 사용한다.

config를 수동 편집하는 기능은 제공하지 않는다. 형식이나 값이 손상되어 필수 필드를 정상적으로 읽지 못하면 데몬은 해당 주기의 정리를 수행하지 않는다. 설정 변경에는 `modify`를 사용하는 것이 안전하다.

## 9. 파일 탐색 및 출력 구조

데몬은 모니터링 디렉터리를 재귀 순회하지만 다음 항목은 복사하지 않는다.

- `ssu_cleanupd.config`
- `ssu_cleanupd.log`
- `-x`로 지정한 디렉터리와 그 하위 항목
- `-e` 필터와 일치하지 않는 파일
- 심볼릭 링크
- 디렉터리 이외의 소켓, FIFO, 장치 파일 등 비일반 파일

일반 파일은 선택된 파일의 확장자 이름을 가진 디렉터리로 복사한다.

```text
모니터링 디렉터리
├── source.c
├── notes.txt
├── LICENSE
└── sub/
    └── report.txt

출력 디렉터리
├── c/
│   └── source.c
├── txt/
│   ├── notes.txt
│   └── report.txt
└── noext/
    └── LICENSE
```

동일 파일명이 여러 하위 경로에 존재하면 `-m` 정책에 따라 하나만 선택하거나 모두 건너뛴다. 출력 파일의 권한 비트와 초 단위 atime/mtime은 선택된 원본을 기준으로 설정한다.

디렉터리 순회에는 파일시스템이 반환한 순서를 사용하므로, 한 주기에 여러 파일이 복사될 때 로그 줄의 세부 순서는 보장하지 않는다.

## 10. log 파일

각 모니터링 디렉터리에 다음 로그 파일이 존재한다.

```text
<DIR_PATH>/ssu_cleanupd.log
```

파일이 실제로 출력 경로에 복사되고 로그 쓰기까지 성공하면 다음 형식으로 한 줄을 추가한다.

```text
[HH:MM:SS][DAEMON_PID][SOURCE_ABSOLUTE_PATH][DESTINATION_ABSOLUTE_PATH]
```

예시:

```text
[20:15:05][12345][/home/user/cleanup-demo/inbox/sub/main.c][/home/user/cleanup-demo/arranged/c/main.c]
```

다음 경우에는 새 로그를 기록하지 않는다.

- 확장자 또는 제외 경로 조건으로 대상에서 제외된 경우
- 중복 모드 3에 의해 건너뛴 경우
- 기존 출력 파일이 모드 1 또는 2의 선택 조건을 이미 충족하는 경우
- 파일 복사에 실패한 경우

`max_log_lines`가 `none`이면 기존 로그에 계속 추가한다. 숫자 N이면 정리 주기 종료 후 최근 N줄만 유지한다. `show`는 이 제한과 관계없이 현재 파일에 남아 있는 줄 중 최근 최대 10줄을 보여준다.

## 11. 처음부터 실행해 보는 테스트

아래 절차는 실제 사용자 파일과 섞이지 않도록 HOME 아래에 전용 테스트 디렉터리를 사용한다.

### 11.1 테스트 데이터 준비

```bash
mkdir -p "$HOME/ssu-cleanupd-demo/inbox/A"
mkdir -p "$HOME/ssu-cleanupd-demo/inbox/ignored"
mkdir -p "$HOME/ssu-cleanupd-demo/arranged"

touch "$HOME/ssu-cleanupd-demo/inbox/readme.txt"
touch "$HOME/ssu-cleanupd-demo/inbox/A/main.c"
touch "$HOME/ssu-cleanupd-demo/inbox/ignored/secret.txt"
touch "$HOME/ssu-cleanupd-demo/inbox/photo.jpg"
```

### 11.2 데몬 등록

프로젝트 디렉터리에서 빌드한 뒤 실행 파일의 절대경로를 보관하고 테스트 디렉터리로 이동해 실행한다. 이렇게 실행하면 SSU-Cleanupd 프롬프트에서 `inbox`, `arranged` 같은 상대경로를 그대로 사용할 수 있다.

```bash
make
SSU_CLEANUPD_BIN="$(pwd)/ssu_cleanupd"
cd "$HOME/ssu-cleanupd-demo"
"$SSU_CLEANUPD_BIN"
```

프롬프트에 다음 명령을 입력한다.

```text
add inbox -d arranged -i 2 -l 5 -x inbox/ignored -e txt,c -m 1
```

2초보다 조금 더 기다린 후 다른 터미널에서 결과를 확인한다.

```bash
find "$HOME/ssu-cleanupd-demo/arranged" -type f -print
```

예상되는 파일은 다음 두 개다.

```text
$HOME/ssu-cleanupd-demo/arranged/txt/readme.txt
$HOME/ssu-cleanupd-demo/arranged/c/main.c
```

`ignored/secret.txt`는 제외 경로 안에 있어 복사되지 않는다. `photo.jpg`는 `-e txt,c`와 일치하지 않아 복사되지 않는다.

### 11.3 상태 조회

관리 셸에서 다음 명령을 입력한다.

```text
show
```

목록에서 테스트 경로의 번호를 입력해 config와 최근 로그를 확인한다. `0`을 입력하면 조회 화면에서 돌아온다.

### 11.4 실행 중 설정 변경

```text
modify inbox -i 5 -l 3 -e c -m 2
```

config에서 `time_interval`, `max_log_lines`, `extension`, `mode`가 변경되고 다른 항목은 유지되는지 `show`로 확인할 수 있다. 이미 진행 중인 대기가 끝난 뒤부터 변경된 설정이 적용된다.

### 11.5 데몬 종료

```text
remove inbox
show
exit
```

`show` 목록에서 테스트 경로가 사라졌는지 확인한다. `remove`는 테스트 파일과 출력 결과를 삭제하지 않으므로, 내용을 확인한 뒤 필요하면 사용자가 직접 테스트 디렉터리를 정리한다.

```bash
rm -r "$HOME/ssu-cleanupd-demo"
```

## 12. 중복 모드 테스트

모드 1과 2의 차이를 확인하려면 같은 이름의 파일을 서로 다른 하위 디렉터리에 만들고 mtime을 다르게 지정한다. 먼저 실행 중인 기존 테스트 데몬이 없는지 확인한다.

```bash
mkdir -p "$HOME/ssu-duplicate-demo/inbox/old"
mkdir -p "$HOME/ssu-duplicate-demo/inbox/new"

touch -t 202601010101 "$HOME/ssu-duplicate-demo/inbox/old/duplicate.c"
touch -t 202602020202 "$HOME/ssu-duplicate-demo/inbox/new/duplicate.c"
```

테스트 디렉터리에서 실행 파일을 다시 실행한다.

```bash
cd "$HOME/ssu-duplicate-demo"
"/absolute/path/to/ssu_cleanupd"
```

`/absolute/path/to/ssu_cleanupd`는 앞에서 빌드한 실행 파일의 실제 절대경로로 바꾼다. 관리 셸에서 모드 1로 등록하면 `new/duplicate.c`가 선택된다.

```text
add inbox -i 2 -m 1
```

기본 출력 위치는 다음과 같다.

```text
$HOME/ssu-duplicate-demo/inbox_arranged/c/duplicate.c
```

모드 2 또는 3을 시험하려면 먼저 `remove`한 다음 출력 디렉터리를 다른 위치로 치우거나 삭제하고 원하는 모드로 다시 등록해야 한다. 기존 출력 파일도 중복 비교에 포함되기 때문이다.

## 13. 오류 처리 요약

| 상황 | 처리 |
|---|---|
| 존재하지 않거나 디렉터리가 아닌 경로 | 오류 출력 후 프롬프트 복귀 |
| HOME 밖으로 해석되는 경로 | `<입력 경로> is outside the home directory` 출력 |
| 접근 권한 부족 | 오류 출력 후 프롬프트 복귀 |
| 이미 모니터링 중인 경로 또는 상·하위 중첩 경로 등록 | 등록 거부 |
| 출력 경로와 모니터링 경로 중첩 | 등록 또는 수정 거부 |
| 잘못된 제외 경로 또는 제외 경로끼리 중첩 | 등록 또는 수정 거부 |
| `-i`, `-l`에 자연수가 아닌 값 | 명령 거부 |
| `-m`에 1~3 이외의 값 | 명령 거부 |
| 옵션 값 누락, 알 수 없는 옵션, 동일 옵션 반복 | 명령 거부 |
| 등록되지 않은 경로의 modify/remove | 오류 출력 |
| show에서 잘못된 번호 입력 | 오류 문구 출력 후 다시 선택 |
| 4,096바이트보다 긴 명령 | 나머지 입력을 버리고 오류 출력 |

검증에 실패한 명령은 데몬 등록 또는 config 변경을 진행하지 않는다. 단, 기본 출력 디렉터리는 현재 구현상 최종 데몬 목록 등록 전에 만들어질 수 있으므로, 매우 드문 동시 실행 충돌이 발생하면 비어 있는 `<DIR_PATH>_arranged`가 남을 수 있다.


배포 또는 제출 전에는 실제 대상 Linux 환경에서 다음 최종 확인을 권장한다.

```bash
make clean
make
./ssu_cleanupd
```
