# Mini Unix Shell (msh)

Bu klasör, "Mini Unix Shell" projesinin **başlangıç sürümünü** içerir.
Tek seviyeli pipe (`|`), built-in komutlar (`cd`, `exit`) ve temel
arka plan süreçleri (`&`) eklenmiştir.

## Amaç

POSIX/Linux sistem çağrıları kullanarak komutları yorumlayıp çalıştıran
mini bir kabuk geliştirmek. Bu ilk sürümün amacı, projenin iskeletini
kurmak ve process yaratma + bekleme akışını sağlam bir şekilde oturtmaktır.

## Tasarım

- **REPL (Read-Eval-Print Loop)**:
  1. `fgets` ile satır oku
  2. `|` varsa `parse_pipe` ile iki komuta ayır, `run_pipe` çalıştır
  3. `strtok` ile tokenize et; son token `&` ise argv'den çıkar ve
     komutu background olarak işaretle
  4. Built-in (`cd`, `exit`) ise built-in çalıştır
  5. Foreground komutta `fork()` → çocukta `execvp()`, ebeveynde `waitpid()`
  6. Background komutta ebeveyn `waitpid()` yapmaz; sadece `[pid]`
     yazdırıp prompt'a döner
- **Pipe**: `pipe()` + iki `fork()` + `dup2` ile tek seviyeli pipeline
- **Background akışı ve `SIGCHLD`**:
  1. `SIGCHLD`, `sigaction()` ile kurulmuş handler'a düşer
  2. Handler içinde yalnızca `waitpid(-1, &st, WNOHANG)` çağrısı ile
     biten çocuklar reap edilir ve sonuçlar sabit boyutlu kuyruğa bırakılır
  3. Ana döngü `flush_background_events()` ile bu kuyruğu boşaltır ve
     log'a `bg cmd bitti pid=N exit=K` satırını yazar
  4. Foreground `waitpid()` ile yarış olmaması için foreground komutlarda
     ebeveyn, `SIGCHLD`'yi `sigprocmask()` ile geçici olarak maskeler
- **Loglama**: tüm olaylar `shell.log` dosyasına zaman damgalı yazılır.
  Log yazımı `pthread_mutex` ile korunur; ileride birden fazla
  thread/child aynı anda loga yazsa bile satırlar bozulmaz.

Dosya yapısı:

```
unix-shell/
├── Makefile
├── README.md
├── include/
│   ├── log.h
│   ├── parser.h
│   └── executor.h
└── src/
    ├── main.c       # REPL döngüsü
    ├── log.c        # zaman damgalı, mutex'li log dosyası yazımı
    ├── parser.c     # komut satırını argv dizisine ayırma
    └── executor.c   # fork + execvp + waitpid + SIGCHLD/reaping akışı
```

## Kullanılan Sistem Programlama Kavramları

- **Process management**: `fork()`, `execvp()`, `waitpid()`, `_exit()`
- **System calls / POSIX API**: `isatty`, `getpid`, `localtime_r`, `strerror`, `chdir`, `getcwd`, `getenv`, `setenv`, `pipe`, `dup2`, `close`, `sigaction`, `sigprocmask`
- **Senkronizasyon**: `pthread_mutex_lock/unlock` (log dosyası için)
- **Hata yönetimi**: `errno`, `perror`, log dosyasına seviye etiketli
  (`INFO`, `WARN`, `ERROR`) kayıt

## Çalıştırma Adımları

```bash
cd unix-shell
make            # msh ikilisini üretir
./msh           # interaktif kabuğu başlatır
# veya:
make run
```

Örnek oturum:

```
$ ./msh
msh> ls
Makefile  README.md  shell.c
msh> echo merhaba dunya
merhaba dunya
msh> uname -a
Linux ...
msh> ls | wc -l
8
msh> sleep 5 &
[12345]
msh> echo prompt geri geldi
prompt geri geldi
msh> echo merhaba dunya | tr a-z A-Z
MERHABA DUNYA
msh> exit 3       # 3 koduyla çıkış
```

Kapatmak için `Ctrl-D` (EOF) gönderin. Loglar `shell.log` dosyasında
birikir; sıfırlamak için `make clean` yeterlidir.

## Testler

Bu sürümde manuel test seti:

| # | Komut          | Beklenen davranış                                  |
|---|----------------|-----------------------------------------------------|
| 1 | `ls`           | Dizin listelenir, log'a `exit=0` düşer              |
| 2 | `echo selam`   | "selam" yazılır                                     |
| 3 | `pwd`          | Çalışılan dizin yazılır                             |
| 4 | `false`        | Çıkış kodu 1, log'a `exit=1` düşer                  |
| 5 | `yokboyle`     | "shell: yokboyle: No such file or directory" hatası |
| 6 | (boş satır)    | Yeni prompt, hata yok                               |
| 7 | `Ctrl-D`       | Shell temiz şekilde kapanır                         |
| 8 | `exit`         | Son komutun çıkış koduyla kapanır                   |
| 9 | `exit 4`       | 4 koduyla kapanır, `$?` = 4                         |
| 10| `exit abc`     | "numeric argument required" hatasi, shell kapanmaz   |
| 11| `ls \| wc -l`  | Dosya sayisini yazar, log'a pipe-left/pipe-right duser |
| 12| `echo a \| tr`  | "a" harfi buyur                                      |
| 13| `\| ls`         | "invalid pipe command" hatasi, shell kapanmaz         |
| 14| `ls \|`         | "invalid pipe command" hatasi, shell kapanmaz         |
| 15| `ls \| cd /tmp`  | cd pipe icinde calisir, ebeveyn dizini degismez     |
| 16| `echo \| exit 5` | pipe icindeki exit sadece cocuk prosesi oldurur     |
| 17| `sleep 1 &`      | Prompt hemen doner, `[pid]` yazilir                 |
| 18| `sleep 1 &` + log| Bitince log'a `bg cmd bitti pid=N exit=0` duser    |
| 19| `ps -o stat= -p N` | Proses bitince `Z` gorulmez, zombie kalmaz        |
| 20| `cd /tmp &`      | Built-in background reddedilir, shell calismaya devam eder |

Hızlı toplu test:

```bash
printf 'echo merhaba\nsleep 1 &\nls | wc -l\nfalse\nexit 3\n' | ./msh
cat shell.log
```

## Karşılaşılan Problemler

- **`exec` sonrası kontrol akışı**: `execvp` başarılıysa zaten geri
  dönmüyor; başarısız olduğunda `fprintf` + `_exit(127)` ile çocuk
  süreç kapatıldı (ebeveynin kabuğuna düşmesin diye `exit` yerine
  `_exit` tercih edildi).
- **Log satırlarının karışması**: Tek thread'de sorun değil ama
  ileride pipeline / arka plan komutları eklenince birden fazla
  süreç aynı `FILE*`'ye yazacak. Şimdilik `pthread_mutex` ile süreç
  içinden korunuyor, sonraki adımda `flock`/`O_APPEND` davranışı
  da değerlendirilecek.
- **`SIGCHLD` içinde güvenli işlem**: Handler içinde `printf` veya
  `log_msg` çağırmak async-signal-safe değil. Bu yüzden handler sadece
  reaping yapıyor; log yazımı ve kullanıcıya görünür yan etkiler ana
  döngüde tamamlanıyor.
- **Boş satır / sadece boşluk**: `parse_line` 0 dönerse `fork`'a hiç
  girilmiyor; aksi halde her enter'da gereksiz process açılırdı.

## Sonraki Adımlar (TODO)

- [x] `cd` ve `exit` built-in komutlari
- [x] Arka plan surecleri (`&`) ve `SIGCHLD` ile reaping
- [x] Tek seviyeli pipe (`|`) destegi
- [ ] Son 10 komut icin history
- [ ] Performans degerlendirmesi (komut basina sure olcumu, ortalamalar)
