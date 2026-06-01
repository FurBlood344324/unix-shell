# Mini Unix Shell (msh)

## Amaç

Bu proje, POSIX/Linux sistem çağrıları kullanarak komutları yorumlayıp çalıştıran
mini bir kabuk (shell) geliştirmeyi amaçlar.

## Tasarım

- **REPL (Read-Eval-Print Loop)**:
  1. `readline` (history.c) ile ham terminal modunda satır oku; ok tuşlarıyla
     geçmiş gezintisi, temel satır düzenleme (Ctrl-A/E/U/W, backspace, delete)
  2. `strtok` ile tokenize et; son token `&` ise argv'den çıkar ve
     komutu background olarak işaretle
  3. `|` karakteri için `parse_pipe` ile komut ikiye ayrılıp `run_pipe` çalıştırılır
  4. Built-in (`cd`, `exit`, `history`) ise `run_builtin` çalıştırılır
  5. Foreground komutta `fork()` → çocukta `execvp()`, ebeveynde `waitpid()`
  6. Background komutta ebeveyn `waitpid()` yapmaz; sadece PID'yi log'a yazıp
     prompt'a döner
- **Pipe**: `pipe()` + iki `fork()` + `dup2` ile tek seviyeli pipeline
- **Background akışı ve `SIGCHLD`**:
  1. `SIGCHLD`, `sigaction()` ile kurulmuş handler'a düşer
  2. Handler içinde yalnızca `waitpid(-1, &st, WNOHANG)` çağrısı ile
     biten çocuklar reap edilir ve sonuçlar sabit boyutlu kuyruğa bırakılır
  3. `flush_background_events()` bu kuyruğu boşaltır ve
     log'a çıkış koduyla birlikte süreç tamamlanma satırını yazar
  4. Foreground `waitpid()` ile yarış olmaması için foreground komutlarda
     ebeveyn, `SIGCHLD`'yi `sigprocmask()` ile geçici olarak maskeler
- **Loglama**: tüm olaylar `shell.log` dosyasına zaman damgalı yazılır.
  Log yazımı `pthread_mutex` ile korunur; ileride birden fazla
  thread/child aynı anda loga yazsa bile satırlar bozulmaz.
- **History**: Girilen komutlar halka bir tamponda (ring buffer, kapasite 10) tutulur.
  `history` komutu ile listelenir. Ok tuşları ile geçmişte gezinilebilir.
  Geçmiş `history.txt` dosyasına kaydedilir ve başlangıçta geri yüklenir.

Dosya yapısı:

```
unix-shell/
├── Makefile
├── README.md
├── include/
│   ├── executor.h
│   ├── history.h
│   ├── log.h
│   ├── parser.h
│   └── performance.h
└── src/
    ├── main.c         # REPL döngüsü
    ├── executor.c     # fork + execvp + waitpid + pipe + SIGCHLD/reaping akışı
    ├── parser.c       # komut satırını argv dizisine ayırma
    ├── log.c          # zaman damgalı, mutex'li log dosyası yazımı
    ├── history.c      # readline + komut geçmişi (ring buffer, ham terminal)
    └── performance.c  # komut başına süre ölçümü ve özet
```

## Kullanılan Sistem Programlama Kavramları

- **Process management**: `fork()`, `execvp()`, `waitpid()`, `_exit()`
- **System calls / POSIX API**: `isatty`, `getpid`, `localtime_r`, `strerror`, `chdir`, `getcwd`, `getenv`, `pipe`, `dup2`, `close`, `sigaction`, `sigprocmask`, `clock_gettime`
- **Senkronizasyon**: `pthread_mutex_lock/unlock` (log dosyası için)
- **Hata yönetimi**: `errno`, `perror`, log dosyasına seviye etiketli
  (`INFO`, `WARN`, `ERROR`, `PERF`) kayıt

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
Makefile  README.md  build  include  msh  shell.log  src
msh> echo merhaba dunya
merhaba dunya
msh> uname -a
Linux ...
msh> ls | wc -l
8
msh> sleep 5 &
msh> echo prompt geri geldi
prompt geri geldi
msh> echo merhaba dunya | tr a-z A-Z
MERHABA DUNYA
msh> history
1: ls
2: echo merhaba dunya
3: uname -a
4: ls | wc -l
5: sleep 5 &
6: echo prompt geri geldi
7: echo merhaba dunya | tr a-z A-Z
msh> exit 3       # 3 koduyla çıkış
```

Kapatmak için `Ctrl-D` (EOF) gönderin. Loglar `shell.log` dosyasında
birikir; sıfırlamak için `make clean` yeterlidir.

## Testler

| #  | Komut              | Beklenen davranış                                                |
|----|--------------------|------------------------------------------------------------------|
| 1  | `ls`               | `.` dizinini listeler                                            |
| 2  | `ls -l /`          | Kök dizini uzun formatta listeler                                |
| 3  | `echo selam`       | "selam" yazılır                                                  |
| 4  | `pwd`              | Çalışılan dizin yazılır                                          |
| 5  | `false`            | Çıkış kodu 1, log'a `exit=1` düşer                               |
| 6  | `yokboyle`         | `shell: No such file or directory` hatası                        |
| 7  | `cd /tmp`          | `/tmp` dizinine geçer, prompt değişmez                           |
| 8  | `cd`               | `HOME` dizinine döner                                            |
| 9  | `cd ~`             | `HOME` dizinine döner                                            |
| 10 | `cd ~/Desktop`     | `HOME/Desktop` dizinine geçer                                    |
| 11 | `cd -`             | `OLDPWD` (önceki dizin) varsa oraya döner ve yolu yazdırır       |
| 12 | `ls \| wc -l`      | `ls` çıktısındaki satır sayısını basar                           |
| 13 | `echo a \| tr`     | "a" harfini basar                                                |
| 14 | `\| ls`             | `shell: komut parse edilemedi` hatası, shell kapanmaz            |
| 15 | `ls \|`             | `shell: komut parse edilemedi` hatası, shell kapanmaz            |
| 16 | `ls \| cd /tmp`    | cd pipe içinde çalışır, ebeveyn dizini değişmez                  |
| 17 | `echo \| exit 5`   | pipe içindeki exit sadece çocuk prosesi öldürür                  |
| 18 | `sleep 3 &`        | Arka plana atar, prompt hemen döner                              |
| 19 | `sleep 1 &` + log  | Bitince log'a `background process N exited with code 0` düşer    |
| 20 | `cd /tmp &`        | Built-in background reddedilir, shell çalışmaya devam eder       |
| 21 | `history`          | Son 10 komutu numaralı olarak listeler                           |
| 22 | `exit`             | Son komutun çıkış koduyla kapanır                                |
| 23 | `exit 123`         | 123 koduyla kapanır                                              |
| 24 | `exit abc`         | "numeric argument required" hatası, shell kapanmaz               |
| 25 | (boş satır)        | Yeni prompt, hata yok                                           |

Hızlı toplu test:

```bash
./msh < test_commands.txt; echo "Cikis: $?"
cat shell.log
```

## Karşılaşılan Problemler

- **`exec` sonrası kontrol akışı**: `execvp` başarılıysa zaten geri
  dönmüyor; başarısız olduğunda `fprintf` + `_exit(127)` ile çocuk
  süreç kapatıldı (ebeveynin kabuğuna düşmesin diye `exit` yerine
  `_exit` tercih edildi).
- **Log satırlarının karışması**: Tek thread'de sorun değil ama
  ileride pipeline / arka plan komutları eklenince birden fazla
  süreç aynı `FILE*`'ye yazacak. Şimdilik `pthread_mutex` ile korunuyor.
- **`SIGCHLD` içinde güvenli işlem**: Handler içinde `printf` veya
  `log_msg` çağırmak async-signal-safe değil. Bu yüzden handler sadece
  reaping yapıyor; log yazımı ve kullanıcıya görünür yan etkiler ana
  döngüde (`flush_background_events`) tamamlanıyor.
- **Boş satır / sadece boşluk**: `parse_line` 0 dönerse `fork`'a hiç
  girilmiyor; aksi halde her enter'da gereksiz process açılırdı.
