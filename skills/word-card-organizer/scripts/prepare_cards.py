#!/usr/bin/env python3
"""Validate word-card JSON and export a two-field Anki TSV; no device writes."""
import argparse
import json
import unicodedata
from pathlib import Path


def validate(data):
    if not isinstance(data, dict) or set(data) != {'schema', 'cards', 'needs_review'}:
        raise ValueError('Expected exactly schema, cards and needs_review')
    if data['schema'] != 'moxinzhi-vocab/1':
        raise ValueError('Unsupported schema')
    if not isinstance(data['needs_review'], list) or not all(isinstance(x, str) for x in data['needs_review']):
        raise ValueError('needs_review must be an array of strings')
    if data['needs_review']:
        raise ValueError('Resolve needs_review before exporting')
    cards = data['cards']
    if not isinstance(cards, list) or not 1 <= len(cards) <= 10:
        raise ValueError('Expected 1 to 10 cards per batch')
    seen = set()
    for i, card in enumerate(cards, 1):
        if not isinstance(card, dict) or set(card) != {'front', 'back'}:
            raise ValueError(f'Card {i}: exactly front and back required')
        for field, limit in [('front', 256), ('back', 768)]:
            value = card[field]
            if not isinstance(value, str) or not value.strip() or value != value.strip():
                raise ValueError(f'Card {i}: invalid {field}')
            if any(unicodedata.category(c).startswith('C') for c in value) or any(x in value for x in ['<', '>', '{{', '[sound:']):
                raise ValueError(f'Card {i}: control characters or markup are not allowed')
            if len(value.encode('utf-8')) > limit:
                raise ValueError(f'Card {i}: {field} exceeds UTF-8 byte limit')
        if len(card['front']) > 64:
            raise ValueError(f'Card {i}: front exceeds 64 characters')
        key = ' '.join(unicodedata.normalize('NFKC', card['front']).casefold().split())
        if key in seen:
            raise ValueError(f'Card {i}: duplicate front; distinguish different senses explicitly')
        seen.add(key)
    return cards


def runtime_text(skill_path):
    text = skill_path.read_text(encoding='utf-8')
    if not text.startswith('---\n'):
        raise ValueError('Missing canonical skill frontmatter')
    body = text.split('---\n', 2)[2].lstrip()
    if not body.startswith('# 单词卡整理\n'):
        raise ValueError('Runtime title missing')
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True, help='New directory; existing paths are never overwritten')
    args = parser.parse_args()
    data = json.loads(args.input.read_text(encoding='utf-8'))
    cards = validate(data)
    runtime = runtime_text(Path(__file__).resolve().parents[1] / 'SKILL.md')
    args.output_dir.mkdir(parents=True, exist_ok=False)
    (args.output_dir / 'word-cards.tsv').write_text(''.join(c['front']+'\t'+c['back']+'\n' for c in cards), encoding='utf-8')
    (args.output_dir / 'word-card-organizer.md').write_text(runtime, encoding='utf-8')
    report = {'scope': 'host format validation only; not device execution', 'cards': len(cards), 'max_front_bytes': max(len(c['front'].encode()) for c in cards), 'max_back_bytes': max(len(c['back'].encode()) for c in cards)}
    (args.output_dir / 'validation.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
