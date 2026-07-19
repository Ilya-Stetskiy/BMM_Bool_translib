#!/usr/bin/env python3
"""sample_iis_nsk_large.py [dest_dir]

Скачивает НЕБОЛЬШОЙ репрезентативный поднабор из двух ОЧЕНЬ больших ZIP на
persons.iis.nsk.su/files/persons/epg/ — 100-100k-rnd.zip (550 МБ архив,
~10.2 ГБ распакованные, 1981 запись) и 100-1000k-anf.zip (2.05 ГБ архив,
~98 ГБ распакованные, 661 запись, ТОЛЬКО *.anf) — БЕЗ скачивания архивов
целиком.

Как это работает: оба сервера отдают Accept-Ranges: bytes; ZIP хранит
central directory в конце файла, поэтому Range-запросом можно получить
список всех файлов/размеров, а затем скачать HTTP Range-запросом только
байты конкретной записи (её локальный заголовок + сжатые данные) — не весь
архив. Сжатие внутри — LZMA (zip method 14, поддерживается модулем
zipfile из стандартной библиотеки Python 3.3+, но НЕ старым Info-ZIP
`unzip`).

Выбор файлов — по одному представителю (инстанс .01) из крайних и средних
точек по n/плотности/числу мономов в каждом архиве, чтобы иметь
разнообразие, не скачивая сотни ГБ. См. полный список семейств в
benchmarks/data/iis-nsk/README нет — см. bmm_translib_external_datasets.md
в памяти проекта, если она у вас есть, либо просто перечислите
zf.namelist() через тот же приём.
"""
import io
import os
import sys
import zipfile
import urllib.request


class HTTPRangeFile(io.RawIOBase):
    def __init__(self, url):
        self.url = url
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req) as resp:
            self.size = int(resp.headers.get("Content-Length"))
        self.pos = 0

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_SET:
            self.pos = offset
        elif whence == io.SEEK_CUR:
            self.pos += offset
        elif whence == io.SEEK_END:
            self.pos = self.size + offset
        return self.pos

    def tell(self):
        return self.pos

    def seekable(self):
        return True

    def readable(self):
        return True

    def read(self, n=-1):
        if n is None or n < 0:
            end = self.size - 1
        else:
            if n == 0:
                return b""
            end = min(self.pos + n - 1, self.size - 1)
        if self.pos > end:
            return b""
        req = urllib.request.Request(self.url)
        req.add_header("Range", f"bytes={self.pos}-{end}")
        with urllib.request.urlopen(req) as resp:
            data = resp.read()
        self.pos += len(data)
        return data


SAMPLES = {
    "https://persons.iis.nsk.su/files/persons/epg/100-100k-rnd.zip": {
        "out": "100-100k-rnd-sample",
        "names": [
            "100-100k-rnd/10-100x90-1000.01.anf",
            "100-100k-rnd/10-100x90-1000.01.anf.bdd.bdd",
            "100-100k-rnd/10-100x90-1000.01.anf.zdd.zdd",
            "100-100k-rnd/50-200x50-500.01.anf",
            "100-100k-rnd/50-200x50-500.01.anf.bdd.bdd",
            "100-100k-rnd/10-316x90-316.01.anf",
            "100-100k-rnd/10-316x90-316.01.anf.bdd.bdd",
            "100-100k-rnd/90-316x10-316.01.anf",
            "100-100k-rnd/90-316x10-316.01.anf.bdd.bdd",
        ],
    },
    "https://persons.iis.nsk.su/files/persons/epg/100-1000k-anf.zip": {
        "out": "100-1000k-anf-sample",
        "names": [
            "100-1000k-anf/86-100x14-10000.01.anf",
            "100-1000k-anf/50-250x50-4000.01.anf",
            "100-1000k-anf/50-500x50-2000.01.anf",
            "100-1000k-anf/11-1000x89-1000.01.anf",
            "100-1000k-anf/89-1000x11-1000.01.anf",
        ],
    },
}


def main():
    dest_root = sys.argv[1] if len(sys.argv) > 1 else "benchmarks/data/iis-nsk"
    for url, spec in SAMPLES.items():
        out_dir = os.path.join(dest_root, spec["out"])
        os.makedirs(out_dir, exist_ok=True)
        f = HTTPRangeFile(url)
        zf = zipfile.ZipFile(f)
        for name in spec["names"]:
            out_path = os.path.join(out_dir, os.path.basename(name))
            if os.path.exists(out_path):
                print(f"  пропущено (уже есть): {name}")
                continue
            data = zf.read(name)
            with open(out_path, "wb") as fh:
                fh.write(data)
            print(f"  ok: {name} ({len(data)} байт) -> {out_path}")
    print("Готово.")


if __name__ == "__main__":
    main()
