"""Guard the DSP files taken from SPEAR against accidental local edits.

The manifest records LF-normalized hashes at the SPEAR commit. fir.cpp's Windows guard
for target_clones is normalized back before hashing. The sync-word state machine is not
vendored: src/frame_sync.* is a modified copy (DC-free sync metric), see PROVENANCE.md.
Run: python -m unittest discover -s tests -p test_spear_sources.py -v
"""
import hashlib
import json
from pathlib import Path
import unittest


class SpearSources(unittest.TestCase):
    def test_vendored_dsp_matches_spear(self):
        root = Path(__file__).resolve().parents[1]
        manifest = json.loads((root/'tests/spear_sources.json').read_text())
        self.assertEqual(manifest['commit'], 'd1611fd408939ad238372d58828639e8fde28400')
        files = {p.relative_to(root/'vendor/spear').as_posix() for p in (root/'vendor/spear').rglob('*') if p.is_file()}
        self.assertEqual(files, set(manifest['sha256']))
        for relative, expected in manifest['sha256'].items():
            with self.subTest(source=relative):
                code = (root/'vendor/spear'/relative).read_text(encoding='utf-8').replace('\r\n', '\n')
                if relative == 'dsp/src/fir.cpp':
                    code = code.replace(
                        '#if defined(_WIN32) || !defined(__GNUC__) || !defined(__x86_64__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)',
                        '#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)')
                self.assertEqual(hashlib.sha256(code.encode('utf-8')).hexdigest(), expected)


if __name__ == '__main__':
    unittest.main()
