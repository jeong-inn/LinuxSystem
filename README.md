# Linux System Programming

C와 Linux/POSIX 시스템 인터페이스를 활용한 시스템 프로그래밍 프로젝트 모음.

파일과 디렉터리 관리, 데몬 프로세스, 파일시스템 내부 구조, 프로세스 실행 제어 등 운영체제와 가까운 계층의 기능을 직접 구현하는 데 초점을 둠.

## Projects

### [SSU-Cleanupd](./ssu_cleanup)

지정한 디렉터리를 주기적으로 탐색하고 파일을 확장자별로 정리하는 데몬 기반 파일 관리 프로그램.

- 모니터링 경로 등록·수정·해제
- 데몬 프로세스 생성 및 시그널 기반 종료
- 재귀 디렉터리 순회와 파일 분류
- 중복 파일 처리 정책과 제외 경로 설정
- 설정 파일 잠금, 실행 로그 및 데몬 목록 관리

`C` `Daemon` `Signal` `File I/O` `Directory Traversal` `File Lock`

### [SSU-EXT2](./ssu_ext2)

ext2 이미지 파일을 마운트하지 않고 직접 해석하는 읽기 전용 파일시스템 탐색 프로그램.

- 슈퍼블록, 그룹 디스크립터, inode 직접 해석
- 직접·단일·이중·삼중 간접 블록 순회
- 이미지 내부 디렉터리 트리 출력
- 일반 파일 내용 및 지정 줄 수 출력
- 손상된 메타데이터와 이미지 범위 검증

`C` `ext2` `Inode` `Block Addressing` `Binary Parsing` `Filesystem`

### [SSU-Score](./ssu_score)

정답 디렉터리를 기준으로 빈칸 답안과 C 프로그램 답안을 자동 채점하는 프로그램.

- 복수 정답과 연산식 동치 비교
- 학생 프로그램 자동 컴파일·실행
- 컴파일 경고 감점, 오류 및 실행 시간 초과 처리
- 대소문자와 공백을 제외한 실행 결과 비교
- 배점표·채점 결과 CSV 및 학생별 오답 관리

`C` `Process Control` `POSIX Spawn` `Syntax Tree` `CSV` `Linked List`

### [xv6 Lazy Allocation & Multi-level FS](./xv6-lazy-allocation-multilevel-fs)

xv6의 가상 메모리를 지연 할당 방식으로 확장하고, inode의 블록 주소 체계를 다단계 구조로 재설계한 커널 프로젝트.

- page fault 시점에 물리 페이지를 할당하는 lazy allocation
- 가상·물리 메모리 사용량 조회를 위한 `ssualloc`, `getvp`, `getpp` 시스템 콜
- 미할당 페이지를 고려한 `fork`, `copyout`, 메모리 해제 경로 보완
- 6 direct, 4 single-indirect, 2 double-indirect, 1 triple-indirect 구조
- 다단계 블록의 재귀 해제와 대용량 파일 경계 검증

`C` `xv6` `Virtual Memory` `Page Fault` `System Call` `Inode` `Block Mapping`

### [xv6 Continuous Sector FS](./xv6-continuous-sector-fs)

연속된 디스크 블록을 하나의 extent로 표현하는 Continuous Sector 파일 형식을 xv6에 추가한 파일시스템 프로젝트.

- `O_CS` 플래그와 `T_CS` inode 유형을 통한 일반 파일과의 공존
- 시작 블록 24비트와 길이 8비트를 조합한 extent 인코딩
- 연속 할당 시 extent 확장, 불연속 할당 시 새 entry 생성
- CS 파일 전용 block mapping과 안전한 truncate·삭제 처리
- inode와 direct entry 상태를 확인하는 `printinfo(fd)` 시스템 콜
- 디스크 공간 부족 시 panic 대신 partial/short write 반환

`C` `xv6` `Filesystem` `Extent` `Block Allocation` `Inode` `Error Handling`

## Repository Structure

```text
LinuxSystem/
├── ssu_cleanup/                         # 데몬 기반 파일 정리
├── ssu_ext2/                            # ext2 이미지 분석
├── ssu_score/                           # 답안 자동 채점
├── xv6-lazy-allocation-multilevel-fs/  # 지연 메모리 할당과 다단계 inode
└── xv6-continuous-sector-fs/            # extent 기반 연속 블록 파일
```

빌드·실행 방법과 명령어 옵션은 각 프로젝트 디렉터리의 `README.md` 참고.

## Topics

- Linux/POSIX 시스템 호출과 파일 입출력
- 프로세스·데몬·시그널 및 실행 시간 제어
- 디렉터리 순회와 파일 메타데이터 처리
- 파일시스템 온디스크 구조와 블록 주소 해석
- page fault 기반 지연 메모리 할당과 페이지 테이블 관리
- inode 다단계 주소 지정과 extent 기반 연속 블록 할당
- 연결 리스트 기반 데이터 관리와 정렬
- 경로·입력값·손상 데이터에 대한 예외 처리
