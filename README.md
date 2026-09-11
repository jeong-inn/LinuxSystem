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

## Repository Structure

```text
LinuxSystem/
├── ssu_cleanup/   # 데몬 기반 파일 정리
├── ssu_ext2/      # ext2 이미지 분석
└── ssu_score/     # 답안 자동 채점
```

빌드·실행 방법과 명령어 옵션은 각 프로젝트 디렉터리의 `README.md` 참고.

## Topics

- Linux/POSIX 시스템 호출과 파일 입출력
- 프로세스·데몬·시그널 및 실행 시간 제어
- 디렉터리 순회와 파일 메타데이터 처리
- 파일시스템 온디스크 구조와 블록 주소 해석
- 연결 리스트 기반 데이터 관리와 정렬
- 경로·입력값·손상 데이터에 대한 예외 처리
