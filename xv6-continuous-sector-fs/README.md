# xv6 Continuous Sector File System

> 구현 과정에서 실제로 다시 고친 결함과 설계 단계의 문제 해결 기록은 [TROUBLESHOOTING.md](./TROUBLESHOOTING.md)에서 확인할 수 있습니다.

MIT `xv6-public` x86에 기존 파일 형식을 유지하면서 **Continuous Sector(CS) 기반 파일 형식**을 추가한 운영체제 과제입니다. CS 파일은 연속된 디스크 블록을 `(시작 블록 번호, 길이)` 한 쌍으로 묶어 inode의 direct entry 하나에 저장합니다.

이 저장소는 다음 두 기능을 구현합니다.

1. `O_CS`로 생성하는 CS 파일의 할당·읽기·삭제
2. 파일 descriptor로 inode와 direct block 정보를 출력하는 `printinfo(fd)`

## 요구사항

### 기존 파일시스템과의 공존

- 기존 `T_DIR`, `T_FILE`, `T_DEV`와 일반 파일의 direct/indirect block 동작은 유지합니다.
- CS 파일을 나타내는 `T_CS(4)`를 별도로 추가합니다.
- `open()`에 `O_CREATE | O_CS`가 전달된 경우에만 새 inode를 `T_CS`로 생성합니다.
- CS 방식은 파일에만 적용하며 CS directory는 만들지 않습니다.

### CS entry 형식

기존 inode의 `uint addrs[NDIRECT+1]` 크기는 변경하지 않습니다. CS inode에서는 direct entry 12개만 다음처럼 재해석하고 indirect entry는 사용하지 않습니다.

```text
31                         8 7                         0
+---------------------------+---------------------------+
|  starting block (24 bits) |      length (8 bits)      |
+---------------------------+---------------------------+
```

```c
encoded = (starting_block << 8) | length;
starting_block = encoded >> 8;
length = encoded & 0xFF;
```

명세 본문에는 1-byte 길이를 256개라고 설명하지만 제공 출력은 260개 block을 `255 + 5`로 나눕니다. `0`은 사용하지 않은 entry를 뜻하므로 이 구현은 예시와 8-bit 양의 길이에 맞춰 한 CS의 최대 길이를 **255 blocks**로 처리합니다.

### 할당 규칙

- 기존 extent의 마지막 block 바로 다음 block이 할당되고 길이가 255 미만이면 같은 CS의 길이를 늘립니다.
- 다음 free block이 연속하지 않거나 현재 CS 길이가 255이면 다음 빈 direct entry에 새 CS를 만듭니다.
- 12개의 direct entry를 모두 사용하거나 disk data block이 부족하면 가능한 범위까지만 기록하고 오류를 출력합니다.
- 같은 파일의 기존 block 읽기와 overwrite는 해당 논리 block이 속한 CS를 찾아 처리합니다.

### 삭제 규칙

CS 파일의 link count가 0이 되면 각 entry의 시작 block부터 `length`개 block을 bitmap에서 모두 해제합니다. CS inode에서는 indirect block을 할당하거나 해제하지 않습니다.

### `printinfo(fd)` 출력

출력 순서와 표기는 명세의 형식을 따릅니다.

```text
FILE NAME: test_cs
INODE NUM: 20
FILE TYPE: CS
FILE SIZE: 133120 Bytes
DIRECT BLOCK INFO:
[0] 182015 (num: 710, length: 255)
[1] 247045 (num: 965, length: 5)
```

- 일반 파일은 `FILE TYPE: FILE`과 사용 중인 direct block 번호만 출력합니다.
- CS 파일은 encoded entry 값과 `(num, length)`를 함께 출력합니다.
- 원래 xv6의 inode와 open-file 구조에는 파일명이 없으므로, `open()` 시 사용한 경로의 마지막 요소를 open-file 객체에 보존합니다.

## 구현 구조

### 파일 생성

```text
open(path, O_CREATE | O_CS | O_RDWR)
  → sys_open()
  → create(path, T_CS, ...)
  → T_CS inode 생성
```

### 쓰기와 block mapping

```text
write()
  → filewrite()
  → writei()
  → bmap()
  → T_CS이면 csbmap()
      ├─ 기존 CS에서 논리 block 검색
      ├─ first-fit으로 새 data block 할당
      ├─ 직전 block과 연속이면 length 증가
      └─ 불연속이거나 length=255이면 새 entry 생성
```

### 삭제

```text
unlink()
  → iput()
  → itrunc()
  → T_CS이면 모든 (start, length) 구간 순회
  → data block 해제 및 entry 초기화
```

## 주요 변경 파일

| 파일 | 변경 내용 |
|---|---|
| `stat.h` | `T_CS` 추가 |
| `fcntl.h` | `O_CS` 추가 |
| `fs.h` | CS encoding 상수와 최대 block 수 정의 |
| `fs.c` | CS mapping, 안전한 block 부족 처리, CS-aware `itrunc`, read/write 연결 |
| `file.h`, `file.c` | 출력용 파일명 보존, short write 처리 |
| `sysfile.c` | CS inode 생성과 `sys_printinfo()` 구현 |
| `syscall.h`, `syscall.c` | `printinfo` syscall 번호와 dispatch 등록 |
| `user.h`, `usys.S` | user API와 syscall stub 추가 |
| `ls.c` | 경로로 지정한 `T_CS` 파일도 일반 파일처럼 표시 |
| `cs_test.c` | 연속·불연속 할당, read-back, unlink 자체 시험 |
| `Makefile` | `_cs_test` 포함, 원본 `test.c`가 있으면 `_test` 자동 포함 |

## 빌드와 실행

32-bit x86 ELF toolchain과 QEMU가 필요합니다. Linux 환경에서 다음 명령을 사용합니다.

```bash
make clean
make
make qemu-nox
```

xv6 shell에서 자체 시험을 실행합니다.

```text
$ cs_test
```

시험은 다음 순서로 동작합니다.

1. CS 파일에 130KB를 연속 기록하고 `255 + 5` extent 분할 확인
2. 파일을 다시 열어 130KB 전체 read-back 확인
3. 파일을 삭제해 CS 전용 `itrunc()` 실행
4. 다른 CS 파일에 51KB 기록
5. 일반 파일에 2KB를 끼워 넣어 disk block 연속성 중단
6. CS 파일에 나머지 79KB 기록
7. 일반 파일과 CS 파일의 `printinfo()` 출력
8. 두 파일 read-back 및 삭제 확인

## 현재 검증 상태

| 검증 항목 | 상태 |
|---|---|
| 변경된 kernel/user C 파일의 i386 freestanding 문법·경고 검사 | 통과 |
| 변경된 inode header로 `mkfs` 빌드 및 1,000-block image 생성 | 통과 |
| 전체 xv6 kernel/user program link | 현재 호스트에 Linux i386 linker가 없어 미실행 |
| QEMU 부팅 및 `cs_test` 실행 | 현재 호스트에 QEMU가 없어 미실행 |
| 교수 제공 원본 `test.c` | 첨부 파일에 없어 미실행 |

원본 `test.c`를 이 디렉터리에 수정하지 않고 복사하면 Makefile이 `_test`를 자동으로 `fs.img`에 포함합니다. 제출 전에는 반드시 수업의 x86 Linux/QEMU 환경에서 `make`, `cs_test`, 원본 `test`를 다시 실행해야 합니다.

## 제한사항

- 명세 범위대로 CS directory는 지원하지 않습니다.
- CS inode는 indirect block을 사용하지 않습니다.
- CS 파일당 최대 표현 용량은 `12 × 255 × 512 = 1,566,720 bytes`입니다. 실제 저장 가능량은 filesystem image의 free block 수에 제한됩니다.
- open-file 객체가 보존하는 이름은 `open()`에 사용된 마지막 path component입니다. hard link가 있는 경우 inode의 유일한 이름을 뜻하지 않습니다.
- 저장 후 임의 위치 수정은 명세의 필수 고려 대상이 아니지만, 이미 매핑된 범위의 overwrite는 동작하도록 구현했습니다.

## 기반

- Base: MIT `xv6-public` x86
- Base commit: `eeb7b41`
- Assignment specification: `22-OS-Project#5-V1.1.pdf`
