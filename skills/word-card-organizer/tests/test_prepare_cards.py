import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('prepare_cards', Path(__file__).resolve().parents[1] / 'scripts/prepare_cards.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def payload(cards=None):
    return {'schema': 'moxinzhi-vocab/1', 'cards': cards or [{'front': 'apple', 'back': 'n. 苹果。'}], 'needs_review': []}


class FormatTests(unittest.TestCase):
    def test_valid_unicode(self):
        self.assertEqual(len(m.validate(payload())), 1)

    def test_duplicate_rejected(self):
        with self.assertRaises(ValueError):
            m.validate(payload([{'front': x, 'back': 'n. 苹果。'} for x in ['apple', 'APPLE']]))

    def test_senses_preserved(self):
        self.assertEqual(len(m.validate(payload([{'front': x, 'back': 'n. 释义'} for x in ['bank (river)', 'bank (finance)']]))), 2)

    def test_injection_not_exported(self):
        for x in ['x\ty', 'x\ny', '<script>x</script>', '{{c1::word}}', 'x\x00y']:
            with self.subTest(x=x), self.assertRaises(ValueError):
                m.validate(payload([{'front': 'test', 'back': x}]))

    def test_utf8_and_character_limits(self):
        for card in [{'front': 'x'*65, 'back': 'test'}, {'front': 'test', 'back': '中'*257}]:
            with self.subTest(card=card), self.assertRaises(ValueError):
                m.validate(payload([card]))

    def test_pending_review_stops_export(self):
        d = payload();d['needs_review'] = ['请确认词义']
        with self.assertRaises(ValueError):m.validate(d)

    def test_bad_schema_and_count(self):
        for d in [{'schema': 'wrong', 'cards': [], 'needs_review': []}, payload([{'front': str(i), 'back': 'test'} for i in range(11)])]:
            with self.subTest(d=d), self.assertRaises(ValueError):m.validate(d)

    def test_runtime_format(self):
        text=m.runtime_text(Path(__file__).resolve().parents[1]/'SKILL.md')
        self.assertTrue(text.startswith('# 单词卡整理\n'))
        self.assertNotIn('---\nname:', text)


if __name__ == '__main__':unittest.main()
