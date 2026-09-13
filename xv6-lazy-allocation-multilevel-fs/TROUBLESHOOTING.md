# xv6 과제 4 트러블슈팅 기록

이 문서는 지연 물리 메모리 할당과 Multi-level File System을 구현하면서 해결해야 했던 기술적 문제를 `문제 → 원인 → 해결 → 검증` 순서로 정리합니다.

## 증거 수준

현재 보존 자료에는 완성 코드와 재실행 결과가 있지만 수업 당시의 panic 화면, 수정 전 코드와 commit 이력은 없습니다. 따라서 아래 표현을 구분합니다.

- **실행 확인:** 현재 보존 코드로 QEMU에서 결과를 확인한 항목
- **코드 확인:** 해결 구조가 코드에 존재하지만 당시 실패 화면은 없는 항목
- **추가 검증:** 코드 또는 계산만 확인했고 해당 경계의 실행 완료 결과가 없는 항목

관찰하지 않은 panic이나 오류를 과거에 직접 겪은 사실처럼 만들지 않습니다.

## 요약

| # | 문제 | 핵심 원인 | 해결 | 증거 수준 |
|---:|---|---|---|---|
| 1 | Lazy allocation 이후 기존 VM 경로의 전제 붕괴 | `proc->sz` 아래 page가 모두 present라는 가정 | `PTE_SSU` 상태와 `copyuvm·deallocuvm·uva2ka·copyout` 호환 처리 | 코드·통합 실행 확인 |
| 2 | Page fault를 잘못 처리하면 반복 fault 또는 잘못된 주소 허용 | fault 주소 정렬, 원인·권한 검사와 TLB 갱신 필요 | CR2·error code 검사, page 단위 할당, zero-fill, PTE 갱신, CR3 reload | 메모리 시퀀스 실행 확인 |
| 3 | direct→single→double→triple 경계에서 index 오류 위험 | 계층별 용량과 나눗셈 기준이 다름 | 논리 block 범위를 먼저 분리하고 단계별 index 계산 | double 구간까지 실행 확인 |
| 4 | 파일 삭제 시 data block만 지우면 metadata block 누수 | 할당 tree와 기존 `itrunc()`의 해제 구조 불일치 | level 기반 재귀 해제로 data·index block 모두 회수 | double 구간까지 unlink 확인 |
| 5 | 2,500,000-block image 생성 비용 | 모든 block을 실제 0으로 쓰는 초기화 | sparse image와 multi-level `mkfs::iappend()` | image 생성 코드·빌드 확인 |

## 1. Lazy page가 기존 VM 경로의 전제를 깨는 문제

### 문제와 영향

기존 xv6는 `proc->sz`보다 작은 주소라면 해당 page가 실제 물리 메모리에 존재한다고 가정합니다. `ssualloc()`이 주소 공간만 늘리고 물리 page를 나중에 할당하면 다음 상태가 새로 생깁니다.

```text
process size 안의 합법적인 주소
└─ page table entry는 존재
   └─ 아직 PTE_P가 없고 물리 page도 없음
```

이 상태를 단순한 invalid page와 구분하지 않으면 fork, 주소 공간 축소와 system call의 user buffer 처리에서 잘못된 오류 또는 panic이 발생할 수 있습니다.

### 원인

- 기존 page table에는 `합법적으로 예약됐지만 non-present인 page`를 나타내는 상태가 없습니다.
- `copyuvm()`은 대상 PTE가 present가 아니면 잘못된 page table로 간주합니다.
- `deallocuvm()`은 실제 page만 해제하는 구조라 예약 PTE가 남을 수 있습니다.
- `uva2ka()`와 `copyout()`은 사용자 주소가 즉시 kernel virtual address로 변환된다고 가정합니다.

### 해결

PTE의 software-available bit 하나를 `PTE_SSU`로 정의했습니다.

```c
#define PTE_SSU 0x200
```

- `ssualloc()`은 `PTE_SSU | PTE_U | PTE_W`만 기록하고 `PTE_P`는 설정하지 않습니다.
- `copyuvm()`은 예약 PTE를 자식에게도 예약 상태로 복제합니다.
- `deallocuvm()`은 present page뿐 아니라 `PTE_SSU` entry도 제거합니다.
- `uva2ka()`는 page table 또는 PTE가 없을 때 null pointer를 역참조하지 않습니다.
- `copyout()`은 목적지가 예약 page라면 먼저 한 page를 물리화한 뒤 복사합니다.
- system call 처리 중 kernel mode에서 예약 user page에 접근해 발생한 fault도 같은 검증 경로를 사용합니다.

### 검증

`ssualloc_test`에서 가상 page와 물리 page가 다음 순서로 변하는 것을 확인했습니다.

```text
3/3 → 4/3 → 4/4 → 7/4 → 7/5 → 7/6 → 7/7
```

예약 직후에는 가상 page만 늘고 각 page를 처음 접근할 때 물리 page가 하나씩 증가했습니다.

### 작성 경계

호환 경로의 구현과 최종 실행은 확인했지만 당시 `copyuvm panic`이나 `copyout 실패` 화면은 없습니다. 자기소개서에서는 `기존 VM 경로가 새 상태를 처리하도록 함께 수정했다`고 쓰고, 특정 panic을 직접 재현했다고 쓰지 않습니다.

## 2. Page fault 이후 실행을 안전하게 재개하는 문제

### 문제와 영향

Trap 14가 발생했다는 사실만 보고 page를 할당하면 null pointer, protection fault와 kernel address 접근까지 정상적인 lazy allocation으로 오인할 수 있습니다. 반대로 PTE 갱신 후 TLB가 이전 non-present 상태를 계속 사용하면 같은 주소에서 fault가 반복될 수 있습니다.

### 해결

1. `rcr2()`로 fault 가상 주소를 읽습니다.
2. `PGROUNDDOWN()`으로 page 시작 주소에 맞춥니다.
3. page fault error code에서 non-present fault인지 확인합니다.
4. 주소가 user 영역이고 `proc->sz` 안인지 확인합니다.
5. PTE가 `PTE_SSU` 상태인지 확인합니다.
6. `kalloc()`으로 정확히 한 page를 할당하고 전체를 0으로 초기화합니다.
7. PTE를 `PTE_P | PTE_W | PTE_U`로 갱신합니다.
8. CR3를 다시 로드해 TLB를 갱신하고 fault 명령으로 돌아갑니다.

### 검증

- 예약 page를 순서와 다르게 접근해도 접근한 page만 하나씩 물리화됐습니다.
- 동일 page의 후속 접근은 추가 물리 page 증가 없이 성공했습니다.
- 잘못된 `ssualloc()` 크기 요청은 거부됐습니다.

### 작성 경계

최종 동작은 확인했지만 수정 전 반복 fault 로그는 없습니다. `반복 page fault를 직접 겪고 해결했다`보다 `반복 fault와 잘못된 주소 허용을 막기 위해 원인·상태·TLB를 함께 검증했다`가 안전한 표현입니다.

## 3. Multi-level block 경계와 index 계산

### 문제와 영향

inode의 13개 주소를 6 direct, 4 single, 2 double, 1 triple indirect로 재구성하면 같은 논리 block 번호도 구간에 따라 필요한 index 수와 계산식이 달라집니다. 경계를 한 칸 잘못 계산하면 다른 data block을 읽거나 metadata block을 data로 해석할 수 있습니다.

### 해결

먼저 구간별 누적 용량을 고정했습니다.

| 영역 | 논리 block 번호 | 용량 |
|---|---:|---:|
| Direct | 0–5 | 6 |
| Single indirect | 6–517 | 512 |
| Double indirect | 518–33,285 | 32,768 |
| Triple indirect | 33,286–2,130,437 | 2,097,152 |

`bmap()`은 앞 구간의 크기를 차례로 뺀 뒤 나눗셈과 나머지로 각 계층의 index를 계산합니다. 필요한 index/data block만 할당하고 수정한 index buffer는 `log_write()`로 journal에 기록합니다.

### 검증

- 5 blocks: direct 구간
- 500 blocks: single indirect 포함
- 5,000 blocks: double indirect 포함

세 크기의 파일에서 write, read-back과 unlink가 통과했습니다.

### 작성 경계

triple indirect의 계산과 코드 구조는 확인했지만 시작점인 33,286 blocks를 넘는 시험은 완료하지 못했습니다. 따라서 `triple indirect를 구현했다`고 쓸 수 있지만 `triple indirect 대용량 실행을 통과했다`고 쓰지 않습니다.

## 4. 계층형 할당과 대칭적인 블록 해제

### 문제와 영향

기존 `itrunc()`는 direct data block과 single indirect block 하나만 해제합니다. 다단계 구조에서 이 코드를 그대로 사용하면 하위 data block 또는 중간 index block이 bitmap에 남아 디스크 공간이 누수될 수 있습니다.

### 해결

`bfreeindirect(dev, block, level)`을 추가해 할당 tree를 leaf부터 재귀적으로 해제했습니다.

```text
data block 해제
→ 가장 아래 index block 해제
→ 상위 index block 해제
→ inode addrs[] 초기화
→ inode size 초기화
```

할당 구조와 해제 구조를 대칭적으로 유지해 single, double과 triple 계층을 같은 규칙으로 처리했습니다.

### 검증

5·500·5,000-block 파일이 생성·읽기 후 정상적으로 삭제됐습니다. 다만 삭제 전후 free-block 개수를 직접 비교한 로그는 없습니다.

## 5. 대용량 파일시스템 이미지 생성

### 문제와 영향

`FSSIZE=2,500,000` blocks를 일반적인 방식으로 모두 0으로 쓰면 image 생성 시간이 매우 길어지고 불필요한 실제 저장 공간을 사용합니다. 또한 host의 `mkfs::iappend()`가 기존 single-indirect 구조만 알면 큰 user program을 새 image에 넣을 수 없습니다.

### 해결

- `ftruncate()`로 논리 크기만 확보하는 sparse image를 생성했습니다.
- 실제 metadata와 초기 파일에 필요한 block만 기록했습니다.
- host `mkfs.c`의 block mapping도 kernel과 동일한 multi-level 구조로 변경했습니다.

### 검증

전체 kernel/user program 빌드와 QEMU 부팅에 성공했고 새 filesystem image에서 자체 test를 실행했습니다. sparse 방식 전후의 생성 시간과 실제 disk 사용량은 정량 측정하지 않았습니다.

## 자기소개서에 가장 적합한 트러블슈팅

가장 강한 소재는 **Lazy allocation이 기존 VM 전체의 상태 전제를 깨뜨린 문제**입니다. 한 syscall만 추가한 것이 아니라 같은 상태를 소비하는 fork, deallocation과 user buffer 경로를 함께 수정했기 때문입니다.

### 안전한 요약

> xv6에 지연 물리 메모리 할당을 추가하면서 기존 커널이 `proc->sz` 아래 page는 모두 present라고 가정한다는 점을 확인했습니다. 예약 page를 invalid page와 구분하기 위해 `PTE_SSU` 상태를 도입하고, page fault 처리뿐 아니라 `copyuvm`, `deallocuvm`, `uva2ka`, `copyout`이 같은 상태를 처리하도록 수정했습니다. 이후 가상·물리 page 수가 `3/3→4/3→4/4→7/4→7/7`로 변하는 시험으로 예약과 실제 할당이 분리됨을 검증했습니다.

### 사용하면 안 되는 표현

- 교수 제공 test를 모두 통과했다.
- triple indirect 대용량 시험을 완료했다.
- 1.02GiB 파일 전체를 생성해 검증했다.
- 당시 `copyuvm panic`, 반복 page fault 또는 bitmap 누수를 직접 관찰했다. 해당 로그를 찾거나 다시 재현하기 전에는 사용하지 않습니다.

## 추가로 확보하면 좋은 증거

- 수업 당시 제출본과 수정본의 `trap.c`, `vm.c`, `fs.c` diff
- QEMU panic 또는 page fault 화면
- 교수 test 결과와 감점 내역
- direct/single/double/triple 경계 직전·직후 시험
- unlink 전후 free-block 개수 비교
- 당시 commit, 보고서와 가장 오래 막혔던 증상 기록
