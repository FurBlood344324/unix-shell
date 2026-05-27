# Mini Unix Shell (msh)

Bu klasör, "Mini Unix Shell" projesinin **başlangıç sürümünü** içerir.
Şu an sadece en temel akış uygulanmıştır: kullanıcıdan komut oku → `fork` →
`execvp` → `waitpid`. Sonraki adımlarda pipe (`|`), arka plan (`&`),
built-in komutlar (`cd`, `exit`) ve history yapısı eklenecektir.

## Amaç

POSIX/Linux sistem çağrıları kullanarak komutları yorumlayıp çalıştıran
mini bir kabuk geliştirmek. Bu ilk sürümün amacı, projenin iskeletini
kurmak ve process yaratma + bekleme akışını sağlam bir şekilde oturtmaktır.

## Tasarım

- Tek dosyalı, tek thread'li bir REPL (Read-Eval-Print Loop):
  1. `fgets` ile satır oku
  2. `strtok` ile boşluğa göre tokenize et
  3. `fork()` → çocukta `execvp()`, ebeveynde `waitpid()`
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
    └── executor.c   # fork + execvp + waitpid akışı
```

## Kullanılan Sistem Programlama Kavramları

- **Process management**: `fork()`, `execvp()`, `waitpid()`, `_exit()`
- **System calls / POSIX API**: `isatty`, `getpid`, `localtime_r`, `strerror`, `chdir`, `getcwd`, `getenv`, `setenv`
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
| 10| `exit abc`     | "numeric argument required" hatası, shell kapanmaz   |

Hızlı toplu test:

```bash
printf 'echo merhaba\nls\nfalse\nyokboyle\nexit 3\n' | ./msh
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
- **Boş satır / sadece boşluk**: `parse_line` 0 dönerse `fork`'a hiç
  girilmiyor; aksi halde her enter'da gereksiz process açılırdı.

## Sonraki Adımlar (TODO)

- [ ] `cd` ve `exit` built-in komutları
- [ ] Arka plan süreçleri (`&`) ve `SIGCHLD` ile reaping
- [ ] Tek seviyeli pipe (`|`) desteği
- [ ] Son 10 komut için history
- [ ] Performans değerlendirmesi (komut başına süre ölçümü, ortalamalar)
