"""The engine's text files: configuration (.ini), localisation (.int), and
KnowWonder's cutscene scripts (System/Cutscenes/*.int).

All three are the same INI layout, sections and key=value lines:

    [Section]
    Key=Value
    ;a comment

A key may repeat, and each line is kept, in order: Engine.int lists
Object= and Preferences= many times over. Values are kept as written,
quotes included; `unquote` strips them.

The encoding differs by file. Most are single byte; a file that starts with
the UTF-16 little endian byte order mark FF FE is UTF-16. Single byte text is
read as Windows-1252, the code page the game was made on, with Latin-1 for any
byte that code page leaves undefined.

Usage: uini.py <file or directory>...                   survey
       uini.py <file> <section> [key]                   look a value up
"""
import sys, os, collections


def decode(b):
    if b[:2] == b'\xff\xfe':
        return b[2:].decode('utf-16-le'), 'utf-16-le'
    if b[:3] == b'\xef\xbb\xbf':
        return b[3:].decode('utf-8'), 'utf-8'
    try:
        return b.decode('cp1252'), 'cp1252'
    except UnicodeDecodeError:
        return b.decode('latin-1'), 'latin-1'


class Ini:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.text, self.encoding = decode(f.read())
        self.sections = collections.OrderedDict()   # name -> [(key, value)]
        self.odd = []                               # lines that are none of the three
        section = None
        for n, line in enumerate(self.text.splitlines(), 1):
            s = line.strip()
            if not s or s.startswith(';') or s.startswith('//'):
                continue
            if s.startswith('[') and s.endswith(']'):
                section = s[1:-1]
                self.sections.setdefault(section, [])
            elif '=' in s and section is not None:
                key, value = s.split('=', 1)
                self.sections[section].append((key.strip(), value.strip()))
            else:
                self.odd.append((n, line))

    def get(self, section, key, default=None):
        """The last value of a key, as the engine reads a single value; keys
        and sections compare without regard to case."""
        for name, entries in self.sections.items():
            if name.lower() == section.lower():
                found = [v for k, v in entries if k.lower() == key.lower()]
                if found:
                    return found[-1]
        return default

    def all(self, section, key):
        """Every value of a repeated key, in order."""
        out = []
        for name, entries in self.sections.items():
            if name.lower() == section.lower():
                out += [v for k, v in entries if k.lower() == key.lower()]
        return out


def unquote(value):
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return value[1:-1]
    return value


def text_files(paths):
    out = []
    for a in paths:
        if os.path.isdir(a):
            for root, _, files in os.walk(a):
                out += [os.path.join(root, f) for f in sorted(files)
                        if f.lower().endswith(('.ini', '.int'))]
        else:
            out.append(a)
    return out


def main(argv):
    if len(argv) >= 2 and os.path.isfile(argv[0]) and not os.path.isdir(argv[1]):
        ini = Ini(argv[0])
        if len(argv) == 2:
            for k, v in [e for name, es in ini.sections.items()
                         if name.lower() == argv[1].lower() for e in es]:
                print('%s=%s' % (k, v))
        else:
            print(ini.get(argv[1], argv[2]))
        return
    files = text_files(argv)
    enc, odd = collections.Counter(), []
    sections = entries = 0
    for f in files:
        ini = Ini(f)
        enc[ini.encoding] += 1
        sections += len(ini.sections)
        entries += sum(len(e) for e in ini.sections.values())
        odd += [(os.path.basename(f), n, line) for n, line in ini.odd]
    print('%d files, %d sections, %d key=value lines; encodings %s'
          % (len(files), sections, entries, dict(enc)))
    print('%d lines that are none of section, key=value or comment' % len(odd))
    for f, n, line in odd[:10]:
        print('  %s:%d: %r' % (f, n, line))


if __name__ == '__main__':
    main(sys.argv[1:])
