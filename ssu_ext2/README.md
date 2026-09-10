# SSU-EXT2

SSU-EXT2는 ext2 파일시스템 이미지 파일을 마운트하지 않고 직접 읽어, 이미지 내부의 디렉터리 구조와 일반 파일 내용을 조회하는 대화형 프로그램이다. 프로그램은 읽기 전용으로 동작하며 이미지 파일을 수정하지 않는다.

## 주요 구현

- ext2 전용 헤더와 라이브러리 없이 슈퍼블록, 그룹 디스크립터, inode, 디렉터리 엔트리를 직접 해석한다.
- 모든 블록 그룹의 inode table을 찾아 inode 번호를 올바른 그룹과 인덱스로 변환한다.
- 1KB, 2KB, 4KB를 포함해 슈퍼블록이 표현할 수 있는 블록 크기를 계산한다.
- 12개 직접 블록과 단일·이중·삼중 간접 블록을 논리 순서대로 읽는다.
- 디렉터리 엔트리를 정렬된 단일 링크드 리스트로 구성한다.
- `tree`, `print`, `help`, `exit` 내장 명령을 제공한다.
- 손상된 길이, inode 번호, 블록 번호, 이미지 범위를 검사해 잘못된 이미지를 무제한으로 읽지 않는다.
- 금지된 `system()`과 `ext2_fs.h`, `libext2fs.h`를 사용하지 않는다.

## 빌드

```bash
make
```

기본 컴파일 옵션은 `-std=c11 -Wall -Wextra -Wpedantic -O2`다. 실행 파일과 오브젝트 파일을 삭제하려면 다음 명령을 사용한다.

```bash
make clean
```

## 실행

```bash
./ssu_ext2 <EXT2_IMAGE>
```

예시:

```bash
./ssu_ext2 ~/ext2disk.img
```

이미지 경로는 프로그램을 실행하는 호스트 셸이 처리하므로 위 명령의 `~`는 셸에서 홈 디렉터리로 확장된다. 이미지가 열리지 않거나, 일반 파일이 아니거나, ext2 매직 번호와 기본 구조가 유효하지 않으면 프롬프트를 시작하지 않고 오류를 출력한다.

정상적으로 이미지를 열면 다음 프롬프트가 나타난다.

```text
20211426>
```

프롬프트에서 사용하는 경로는 호스트 파일시스템 경로가 아니라 ext2 이미지의 루트 inode 2를 기준으로 한 상대경로다. 절대경로와 `..` 구성요소는 허용하지 않는다. `.`과 `./A/B`는 사용할 수 있다.

## 내장 명령

### tree

```text
tree <PATH> [OPTION]...
```

`PATH`가 가리키는 디렉터리와 그 안의 항목을 출력한다. `.`과 `..` 엔트리는 항상 제외하며 이름이 `lost+found`인 엔트리도 트리 출력에서 제외한다. 각 디렉터리 엔트리는 이름 기준 오름차순으로 출력한다.

| 옵션 | 동작 |
|---|---|
| `-r` | 하위 디렉터리를 재귀적으로 출력한다. 생략하면 선택한 디렉터리의 직계 항목만 출력한다. |
| `-s` | 디렉터리와 파일 이름 앞에 inode의 논리 크기를 바이트 단위로 출력한다. |
| `-p` | 파일 유형을 포함한 10자리 권한 문자열을 출력한다. setuid, setgid, sticky 비트도 반영한다. |

옵션은 함께 사용할 수 있다. 다음 입력은 모두 유효하다.

```text
tree . -r -s -p
tree . -rsp
tree A/B -sp
```

크기와 권한을 함께 요청하면 `[권한 크기] 이름` 형식으로 표시한다.

```text
[drwxr-xr-x 4096] .
├── [drwxr-x--- 4096] A
│   └── [-rw-r--r-- 120] main.c
└── [-rw-r--r-- 35] readme.txt

2 directories, 2 files
```

마지막 합계에서 시작 디렉터리도 디렉터리 수에 포함한다. `-r`이 없으면 출력한 직계 항목만 합계에 포함한다.

존재하지 않는 경로 또는 올바르지 않은 옵션에는 tree 사용법을 출력한다. 최종 경로가 존재하지만 디렉터리가 아니면 `Error: '<PATH>' is not directory`를 출력한다.

### print

```text
print <PATH> [OPTION]...
```

이미지 내부의 일반 파일 내용을 바이트 그대로 표준 출력에 쓴다. 파일이 블록 경계에 걸쳐 있거나 간접 블록을 사용하는 경우에도 논리 블록 순서대로 이어서 출력한다. 희소 파일에서 물리 블록 번호가 0인 구간은 0 바이트로 채운다.

```text
print docs/readme.txt
print docs/readme.txt -n 10
```

`-n <line_number>`는 처음부터 지정한 줄 수까지만 출력한다. 줄 수는 1 이상의 10진 정수여야 한다. 줄바꿈은 블록 경계를 넘어가더라도 연속해서 계산하며 빈 줄도 한 줄로 센다. 파일이 줄바꿈으로 끝나지 않으면 마지막 데이터 뒤에 임의의 줄바꿈을 추가하지 않는다.

`-n` 뒤에 값이 없으면 `print: option requires an argument -- 'n'`을 출력한다. 존재하지 않는 경로나 올바르지 않은 옵션에는 print 사용법을 출력한다. 디렉터리, 심볼릭 링크, 장치 등 일반 파일이 아닌 inode에는 `Error: '<PATH>' is not file`을 출력한다.

### help

```text
help
help tree
help print
help help
help exit
```

인자가 없으면 모든 명령의 사용법을 출력하고, 명령 이름을 주면 해당 명령의 사용법만 출력한다. 알 수 없는 이름에는 `invalid command`를 출력한 뒤 전체 도움말을 보여준다.

### exit

```text
exit
```

프로그램을 종료하고 이미지 파일 디스크립터와 동적 메모리를 해제한다.

## ext2 해석 설계

프로그램 시작 시 이미지 오프셋 1,024에서 1,024바이트 슈퍼블록을 읽고 다음 값을 확인한다.

- ext2 매직 번호 `0xEF53`
- 전체 block/inode 수
- block 크기와 group당 block/inode 수
- inode 구조체 크기
- 파일 크기가 가리키는 이미지 범위
- 지원할 수 없는 incompatible feature 비트

블록 크기는 다음 식으로 계산한다.

```text
block_size = 1024 << s_log_block_size
```

그룹 디스크립터 테이블은 `(s_first_data_block + 1) * block_size`에서 읽는다. inode 번호 N은 다음과 같이 그룹과 그룹 내부 인덱스로 나눈다.

```text
group = (N - 1) / s_inodes_per_group
index = (N - 1) % s_inodes_per_group
inode_offset = inode_table[group] * block_size + index * inode_size
```

파일과 디렉터리 데이터는 inode의 15개 블록 포인터를 사용한다.

```text
i_block[0..11] : 직접 블록
i_block[12]    : 단일 간접 블록
i_block[13]    : 이중 간접 블록
i_block[14]    : 삼중 간접 블록
```

디렉터리 데이터 블록은 `inode`, `rec_len`, `name_len`, `file_type`, `name` 순서의 가변 길이 엔트리로 해석한다. `rec_len`의 최소값, 4바이트 정렬, 블록 범위와 이름 길이를 확인한 뒤 살아 있는 엔트리만 링크드 리스트에 추가한다.

## 입력 처리

- 빈 줄은 아무 동작 없이 프롬프트를 다시 출력한다.
- 알 수 없는 내장 명령은 전체 도움말을 출력한다.
- 한 명령의 최대 길이는 4,096바이트이며 최대 256개 토큰을 처리한다.
- 이미지 내부 이름에 공백이 있으면 따옴표 또는 역슬래시를 사용할 수 있다.
- SSU-EXT2 프롬프트는 셸이 아니므로 `$HOME`, `~`, 와일드카드를 확장하지 않는다.

```text
tree "directory with spaces" -r
print directory\ with\ spaces/file.txt -n 3
```

## 테스트 이미지 만들기

Linux에서 빈 ext2 이미지를 생성하려면 다음 명령을 사용할 수 있다.

```bash
dd if=/dev/zero of=ext2disk.img bs=1M count=100
mkfs.ext2 ext2disk.img
mkdir -p "$HOME/mnt/ext2disk"
sudo mount -o loop ext2disk.img "$HOME/mnt/ext2disk"
```

마운트된 디렉터리에 테스트 파일을 만든다.

```bash
sudo mkdir -p "$HOME/mnt/ext2disk/A/B"
printf 'first line\nsecond line\nthird line\n' | sudo tee "$HOME/mnt/ext2disk/A/B/sample.txt" >/dev/null
sudo chmod 640 "$HOME/mnt/ext2disk/A/B/sample.txt"
```

이미지를 분석하기 전에 마운트를 해제해야 한다. 마운트된 상태로 이미지가 변경되는 동안 읽으면 일관되지 않은 데이터를 볼 수 있다.

```bash
sudo umount "$HOME/mnt/ext2disk"
./ssu_ext2 ext2disk.img
```

프로그램 프롬프트에서 다음을 확인한다.

```text
tree . -rsp
tree A -r
print A/B/sample.txt
print A/B/sample.txt -n 2
help tree
exit
```
