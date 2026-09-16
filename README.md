# hentr
henter

## GAIA — мини-ИИ в стиле Horizon

`gaia.py` — чат-бот на базе Claude API (Anthropic), отвечающий в стиле ИИ GAIA
из Horizon Zero Dawn / Forbidden West: спокойно, аналитически, с отсылками к
экосистемам, машинам и восстановлению Земли.

Установка зависимостей:

```bash
pip install -r requirements.txt
```

Запуск (нужен реальный ИИ на базе Claude):

```bash
export ANTHROPIC_API_KEY=sk-ant-...
python3 gaia.py
```

Без `ANTHROPIC_API_KEY` бот работает в ограниченном оффлайн-режиме с
шаблонными ответами.

Для выхода введите `пока` или `exit`.
