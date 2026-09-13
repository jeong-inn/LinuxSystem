# xv6 과제 5 트러블슈팅 기록

이 문서는 Continuous Sector(CS) 기반 파일 시스템 구현 과정에서 확인한 문제와 해결 과정을 정리합니다. 특히 **1차 구현 후 실제로 다시 고친 결함**과 **명세·기존 코드 분석으로 사전에 발견한 충돌**을 구분합니다.

## 증거 수준

- **실제 재수정:** 첫 구현에 넣은 동작을 다시 검토해 결함을 발견하고 코드를 변경한 항목
- **설계 단계 해결:** 명세와 기존 xv6 코드를 대조해 구현 전에 또는 구현 중 충돌을 발견한 항목
- **정적 검증:** i386 대상 C 컴파일 또는 `mkfs`로 확인한 항목
- **실행 미검증:** QEMU와 교수 원본 `test.c`가 없어 결과를 아직 실행으로 확인하지 못한 항목

## 요약

| # | 구분 | 문제 | 해결 | 현재 상태 |
|---:|---|---|---|---|
| 1 | 실제 재수정 | short write 지원 후 0-byte write가 `-1`을 반환 | 전체 성공·부분 성공·빈 요청·완전 실패 반환값 분리 | i386 컴파일 확인 |
| 2 | 실제 재수정 | 용량 경계를 넘는 요청 전체를 거부 | CS 파일은 남은 범위만 기록한 뒤 short write | i386 컴파일 확인 |
| 3 | 설계 단계 해결 | 명세의 256 blocks와 예시의 `255+5` 충돌 | 한 CS의 최대 길이를 255로 결정 | 명세·encoding 대조 |
| 4 | 설계 단계 해결 | 일반 `MAXFILE`로 130KB 예제를 처리할 수 없음 | CS 전용 3,060-block 한도 | 코드 확인 |
| 5 | 설계 단계 해결 | `printinfo(fd)`에서 파일명을 알 수 없음 | open path의 마지막 component를 open-file 객체에 저장 | 코드 확인 |
| 6 | 설계 단계 해결 | block 부족과 short write가 kernel panic으로 전파 | allocator→mapping→inode write→file write의 오류 계약 수정 | 실행 미검증 |
| 7 | 설계 단계 해결 | 일반 `bmap/itrunc`가 encoded CS entry를 오해 | `csbmap`과 CS-aware `itrunc` 분리 | 실행 미검증 |
| 8 | 검증 환경 | Clang에서 기존 GNU designator가 `-Werror` 실패 | 원본 코드는 유지하고 정적 검사에서 기존 확장 경고만 제외 | 컴파일 통과 |

## 실제 1차 구현 후 다시 고친 결함

### 1. 0-byte write 회귀

#### 첫 구현

block 부족 시 일부라도 기록했다면 실제 byte 수를 반환하기 위해 `filewrite()`의 마지막 반환을 다음처럼 변경했습니다.

```c
return i > 0 ? i : -1;
```

#### 발견한 문제

요청 크기 `n`이 0이면 반복문을 실행하지 않아 `i`도 0입니다. 따라서 정상적인 `write(fd, buf, 0)`이 `-1`을 반환하는 회귀가 생깁니다. 부분 실패만 고려하면서 빈 요청이라는 정상 경계값을 놓친 문제입니다.

#### 수정

```c
return i == n ? n : (i > 0 ? i : -1);
```

반환 의미를 다음과 같이 분리했습니다.

| 상황 | 반환값 |
|---|---:|
| 전체 기록 성공 | 요청한 `n` |
| 0-byte 요청 | `0` |
| 일부만 기록 | 실제 기록한 byte 수 |
| 한 byte도 기록하지 못함 | `-1` |

#### 배운 점

실패 처리를 개선하는 변경도 기존 정상 경계 조건을 깨뜨릴 수 있습니다. 반환식 자체보다 `성공·부분 성공·빈 작업·완전 실패`라는 상태를 먼저 나눠야 합니다.

#### 검증 경계

i386 대상 컴파일은 통과했지만 QEMU에서 0-byte write를 호출하는 실행 test는 추가해야 합니다.

### 2. 용량 경계를 넘는 요청 전체를 거부한 문제

#### 첫 구현

CS 파일의 별도 최대 크기를 추가하면서 다음처럼 범위를 검사했습니다.

```c
if(off + n > limit)
  return -1;
```

#### 발견한 문제

과제 명세는 direct block 또는 data block 범위를 넘으면 **범위 안까지는 할당한 뒤 오류를 출력**하도록 요구합니다. 첫 구현은 마지막 일부를 쓸 수 있어도 요청 전체를 거부하는 all-or-nothing 방식이라 요구사항과 맞지 않았습니다.

#### 수정

일반 파일의 기존 동작은 유지하고 CS 파일에만 남은 길이로 요청을 줄였습니다.

```c
if(off + n > limit){
  if(ip->type != T_CS || off >= limit)
    return -1;
  n = limit - off;
  cprintf("csfs: direct block range exceeded\n");
}
```

하위 `writei()`가 실제 기록량을 반환하고 상위 `filewrite()`가 이를 short write로 전달하도록 연결했습니다.

#### 배운 점

요구사항의 `범위 내까지`라는 표현은 단순한 크기 검사가 아니라 부분 성공을 포함하는 API 계약입니다. 경계 검사와 상위 반환 정책을 함께 바꿔야 합니다.

#### 검증 경계

코드는 컴파일됐지만 12개 direct entry 또는 image의 free block을 실제로 소진하는 QEMU 시험은 미실행입니다.

## 명세와 기존 코드 분석으로 해결한 문제

### 3. 256 blocks와 `255+5`의 충돌

#### 문제

명세 본문은 1-byte 길이 영역에 최대 256 blocks를 저장한다고 설명하지만 출력 예시는 260 blocks를 `255+5`로 나눕니다.

#### 분석

- 8-bit unsigned 값의 수학적 범위는 0–255입니다.
- direct entry 전체 값 `0`은 사용하지 않은 entry를 나타냅니다.
- 제공된 기대 출력은 첫 CS의 길이를 255로 표시합니다.

#### 해결

채점 예시와 실제 encoding을 우선해 `CS_MAX_LENGTH=255`로 결정했습니다. 길이가 255인 상태에서 다음 block이 물리적으로 연속하더라도 다음 direct entry에 새 CS를 만듭니다.

#### 작성 경계

명세와 bit 표현을 대조한 판단은 확인됐지만 QEMU 출력에서 실제 `255+5`가 생성되는지는 아직 실행하지 않았습니다.

### 4. 일반 `MAXFILE`과 130KB 시험의 충돌

#### 문제

원본 xv6는 12 direct와 128 indirect blocks를 사용하므로 `MAXFILE=140 blocks`, 약 70KB입니다. 명세의 CS 시험은 130KB, 즉 260 blocks를 요구합니다.

#### 해결

```c
#define CS_MAX_LENGTH 255
#define MAXCSBLOCKS (NDIRECT * CS_MAX_LENGTH)
```

일반 파일은 기존 140-block 한도를 유지하고 CS 파일에만 3,060-block, 1,566,720-byte 구조상 한도를 적용했습니다. 실제 저장 가능량은 1,000-block image의 free block 수가 더 먼저 제한합니다.

### 5. fd만으로 파일명을 출력할 수 없는 문제

#### 문제

`printinfo(int fd)`는 `FILE NAME`을 출력해야 하지만 원본 xv6의 inode와 `struct file`에는 open에 사용한 이름이 없습니다. inode는 hard link 때문에 여러 이름을 가질 수도 있습니다.

#### 검토한 대안

1. root directory를 다시 순회해 같은 inode 번호의 entry 검색
2. 전체 pathname을 inode에 추가
3. `open()`에 사용한 마지막 path component를 open-file 객체에 보존

directory 재탐색은 중첩 경로와 hard link에서 모호하고 on-disk inode 변경은 과도합니다. 세 번째 방법을 선택했습니다.

#### 해결과 제한

`struct file`에 `name[DIRSIZ+1]`을 추가하고 `sys_open()`에서 basename을 저장했습니다. 이 값은 inode의 유일한 이름이 아니라 **해당 fd를 열 때 사용한 이름**입니다.

### 6. `balloc()` panic을 없애도 상위에서 다시 panic하는 문제

#### 문제

명세는 공간이 부족하면 가능한 범위까지만 기록하도록 요구하지만 원본 xv6의 `balloc()`은 free block이 없으면 다음과 같이 종료합니다.

```c
panic("balloc: out of blocks");
```

CS용 allocator가 실패를 반환하도록 바꾸는 것만으로는 충분하지 않았습니다. 상위 `filewrite()`에도 다음 전제가 있기 때문입니다.

```c
if(r != n1)
  panic("short filewrite");
```

#### 해결

```text
ballocx(failok=1)
→ csbmap()이 0 반환
→ writei()가 지금까지 기록한 byte 수 반환
→ filewrite()가 실제 기록량을 user program에 반환
```

일반 파일의 기존 allocator 동작은 유지하고 CS 파일에서만 실패를 복구 가능한 값으로 전달합니다.

#### 직무 연결

한 함수의 예외 처리가 아니라 allocator, block mapping, inode I/O와 file descriptor I/O 사이의 **오류 전파 계약**을 추적한 사례입니다. 시스템 SW·커널·임베디드 안정성 직무에 가장 적합합니다.

### 7. 일반 block pointer와 CS extent의 의미 차이

#### 문제

일반 inode에서 `addrs[i]`는 data block 하나를 가리키지만 CS inode에서는 encoded `(start,length)`입니다. 기존 `bmap()`과 `itrunc()`가 이 값을 block 번호로 사용하면 잘못된 disk block을 읽거나 해제할 수 있습니다.

#### 해결

- `bmap()`은 `T_CS`일 때 `csbmap()`으로 분기합니다.
- `csbmap()`은 extent 길이를 누적해 논리 block이 포함된 구간을 찾습니다.
- append에서 새 block이 `start+length`와 같고 길이가 255 미만일 때만 현재 CS를 늘립니다.
- 물리적 연속성이 끊기거나 길이가 차면 다음 빈 direct entry를 사용합니다.
- `itrunc()`은 entry를 decode한 뒤 `start`부터 `length`개 data block을 각각 해제합니다.
- CS inode의 indirect entry는 사용하지 않습니다.

#### 검증용 시나리오

`cs_test.c`에는 다음 흐름이 구현돼 있습니다.

```text
CS 파일 51KB 기록
→ 일반 파일 2KB 기록
→ 같은 CS 파일에 79KB 추가
→ 두 CS entry 출력 대상 생성
→ read-back
→ unlink
```

코드는 존재하지만 QEMU에서 실제 출력과 block 회수를 확인해야 합니다.

## 검증 환경 트러블슈팅

### 8. Clang `-Werror`와 기존 GNU designator

i386 정적 컴파일 중 원본 xv6의 syscall table 표기가 Clang에서 경고로 처리돼 실패했습니다.

```c
[SYS_fork] sys_fork,
```

이는 추가한 `printinfo` 로직의 오류가 아니라 xv6 원본이 사용하는 GNU의 `missing =` designator 표기입니다. Linux GCC에서는 허용되는 기존 코드를 대규모로 바꾸지 않고, 로컬 Clang 검사에서만 `-Wno-gnu-designator`를 적용했습니다. 이후 수정된 kernel/user C 파일의 i386 freestanding 컴파일이 통과했습니다.

## 현재 검증 결과

| 항목 | 결과 |
|---|---|
| 수정 kernel/user C 파일의 i386 freestanding 컴파일 | 통과 |
| 변경된 header로 `mkfs` 빌드 | 통과 |
| 1,000-block filesystem image 생성 | 통과 |
| 전체 xv6 kernel/user program link | Linux i386 linker 부재로 미실행 |
| QEMU 부팅과 `cs_test` | QEMU 부재로 미실행 |
| 교수 제공 원본 `test.c` | 파일 미확보로 미실행 |

## 자기소개서에 가장 적합한 트러블슈팅

가장 강한 소재는 **block 부족이 kernel panic으로 전파되는 경로를 short write로 바꾼 과정**입니다. `balloc()` 하나가 아니라 `balloc→csbmap→writei→filewrite`의 반환 의미를 함께 분석했기 때문입니다.

### 안전한 요약

> Continuous Sector 파일시스템 요구사항을 기존 xv6 코드와 대조하면서 공간 부족이 `balloc()`의 kernel panic으로 끝나고, 이를 실패 반환으로 바꾸더라도 상위 `filewrite()`가 short write를 다시 panic으로 처리한다는 점을 확인했습니다. allocator부터 file descriptor I/O까지 오류 전달 경로를 추적해 가능한 범위까지 기록하고 실제 기록량을 반환하도록 수정했습니다. 이후 반환식을 다시 검토해 0-byte write가 실패하는 회귀를 찾아 전체 성공·부분 성공·빈 요청·완전 실패를 구분했습니다.

### 현재 작성 경계

위 과정은 구현 코드와 실제 재수정 이력으로 확인되지만 QEMU에서 disk-full panic을 재현한 것은 아닙니다. 따라서 `실행 중 panic을 겪었다`보다 `기존 호출 경로에서 panic 조건을 확인하고 제거했다`고 표현합니다. 사용자가 직접 코드와 test를 검토·수정하고 실행한 뒤 개인 기여 범위를 확정해야 합니다.

## 추가 검증 체크리스트

- [ ] QEMU에서 `cs_test` 전체 통과
- [ ] 130KB 연속 기록 후 extent 길이 `255+5` 확인
- [ ] 51KB CS→2KB 일반 파일→79KB CS 후 extent 분리 확인
- [ ] read-back 내용과 byte 수 확인
- [ ] unlink 전후 free block 회수 확인
- [ ] 0-byte write가 0을 반환하는지 확인
- [ ] disk-full에서 panic 없이 short write와 오류 메시지가 발생하는지 확인
- [ ] direct entry 12개 소진 시 부분 기록 후 정상 반환 확인
- [ ] 교수 제공 원본 `test.c`를 수정하지 않고 실행
