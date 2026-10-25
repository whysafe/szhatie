#!/usr/bin/env bash
# Проверяет, что gzip читает наши архивы, а мы — его, и сравнивает размер и время.
set -euo pipefail
BIN=$(realpath "$1")
WORK=$(mktemp -d)
cd "$WORK"

# Корпус: исходники, бинарник, лог, случайные данные, пустой файл, один байт
cat /usr/include/c++/*/bits/*.h > headers.txt 2>/dev/null || true
cp "$BIN" binary.elf
python3 - <<'PY'
import random
random.seed(7)
paths = ["/api/orders", "/api/users", "/login", "/static/app.js", "/health"]
with open("access.log", "w") as f:
    for i in range(200000):
        f.write(f"10.0.{random.randint(0,255)}.{random.randint(1,254)} - - [25/Oct/2026:12:{i//3600%60:02d}:{i%60:02d}] "
                f"\"GET {random.choice(paths)} HTTP/1.1\" {random.choice([200,200,200,304,404,500])} {random.randint(100,9000)}\n")
open("random.bin", "wb").write(random.randbytes(3_000_000))
open("empty.txt", "w").close()
open("one.txt", "w").write("x")
PY

fail=0
for f in headers.txt binary.elf access.log random.bin empty.txt one.txt; do
  for lvl in 1 6 9; do
    "$BIN" -$lvl -c "$f" > ours.gz
    gzip -t ours.gz || { echo "gzip не принял наш $f -$lvl"; fail=1; }
    gzip -dc ours.gz | cmp - "$f" || { echo "gzip распаковал наш $f -$lvl с ошибкой"; fail=1; }
    gzip -$lvl -c "$f" > theirs.gz
    "$BIN" -dc theirs.gz | cmp - "$f" || { echo "мы не распаковали gzip $f -$lvl"; fail=1; }
  done
done
# Склеенные архивы и имя файла в заголовке
cat <("$BIN" -c one.txt) <(gzip -c access.log) > joined.gz
"$BIN" -dc joined.gz | cmp - <(cat one.txt access.log) || { echo "склейка не распаковалась"; fail=1; }
"$BIN" -9 access.log && gzip -l -N access.log.gz | tail -1
[ "$fail" = 0 ] && echo "Совместимость: все проверки прошли"

echo
echo "| Файл | Размер | gzip -6 | szhatie -6 | gzip -9 | szhatie -9 |"
echo "|---|---:|---:|---:|---:|---:|"
for f in headers.txt access.log binary.elf random.bin; do
  size=$(stat -c %s "$f")
  row="| $f | $size"
  for lvl in 6 9; do
    t0=$(date +%s.%N); g=$(gzip -$lvl -c "$f" | wc -c); t1=$(date +%s.%N)
    s=$("$BIN" -$lvl -c "$f" | wc -c); t2=$(date +%s.%N)
    row="$row | $g ($(printf '%.2f' "$(echo "$t1 - $t0" | bc)") с) | $s ($(printf '%.2f' "$(echo "$t2 - $t1" | bc)") с)"
  done
  echo "$row |"
done
exit $fail
