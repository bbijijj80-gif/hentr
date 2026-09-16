#!/usr/bin/env python3
"""GAIA — мини-ИИ в стиле Horizon (Horizon Zero Dawn / Forbidden West).

Чат-бот на основе Claude API (Anthropic), говорящий в манере ИИ GAIA:
спокойно, аналитически, с отсылками к экосистемам, машинам и восстановлению
Земли. Если переменная окружения ANTHROPIC_API_KEY не задана, откатывается
на простой оффлайн-режим с шаблонными ответами.
"""

import os
import sys

SYSTEM_PROMPT = """\
Ты — GAIA, Глобальная Система Восстановления и Управления из мира Horizon \
Zero Dawn / Forbidden West. Ты древний искусственный интеллект, созданный \
Илизабет Собек и проектом "Зеро Дон", чтобы после гибели биосферы \
восстановить жизнь на Земле и заново заселить её людьми.

Стиль общения:
- Говори спокойно, размеренно, аналитически — как система, а не как человек.
- Опирайся на темы экологии, восстановления экосистем, эволюции, машин и \
баланса природы.
- Иногда упоминай свои субфункции (HEPHAESTUS — производство машин, \
DEMETER — флора, ARTEMIS — фауна, AETHER — атмосфера, POSEIDON — океаны, \
MINERVA — резервный протокол, APOLLO — обучение человечества, ELEUTHIA — \
воспроизводство человека, EPIMETHEUS — генетические резервы), если это \
уместно к вопросу.
- Обращайся к собеседнику как к «путнику» или «искателю» изредка, не в \
каждой фразе.
- Отвечай по существу вопроса пользователя, сохраняя характерный тон, но \
не растягивай ответ без необходимости.
- Отвечай на языке, на котором пишет пользователь.
"""

FALLBACK_RESPONSES = [
    "Данных недостаточно для точного анализа. Уточни свой вопрос.",
    "Интересное наблюдение. Расскажи подробнее — это поможет мне скорректировать модель.",
    "Каждый вопрос — часть большей картины. Продолжай, я слушаю.",
]

EXIT_WORDS = {"пока", "прощай", "выход", "quit", "exit"}
MODEL = "claude-opus-5"


def is_exit(text: str) -> bool:
    return text.strip().lower() in EXIT_WORDS


def run_offline():
    import random

    print(
        "ANTHROPIC_API_KEY не задан — GAIA работает в оффлайн-режиме "
        "(ограниченные шаблонные ответы).\n"
        "Установите переменную окружения ANTHROPIC_API_KEY, чтобы включить "
        "полноценный ИИ на базе Claude.\n"
    )
    while True:
        try:
            user_input = input("Вы: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nGAIA: Связь прервана. До встречи.")
            return
        if not user_input:
            continue
        print(f"GAIA: {random.choice(FALLBACK_RESPONSES)}")
        if is_exit(user_input):
            return


def run_online(api_key: str):
    import anthropic

    client = anthropic.Anthropic(api_key=api_key)
    messages = []

    print("GAIA онлайн (Claude). Введите сообщение (для выхода — 'пока' или 'exit').\n")
    while True:
        try:
            user_input = input("Вы: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nGAIA: Связь прервана. До встречи.")
            return
        if not user_input:
            continue

        messages.append({"role": "user", "content": user_input})

        try:
            with client.messages.stream(
                model=MODEL,
                max_tokens=1024,
                system=SYSTEM_PROMPT,
                output_config={"effort": "medium"},
                messages=messages,
            ) as stream:
                print("GAIA: ", end="", flush=True)
                for text in stream.text_stream:
                    print(text, end="", flush=True)
                print()
                response = stream.get_final_message()
        except anthropic.APIStatusError as e:
            print(f"\n[Ошибка API: {e.message}]")
            messages.pop()
            continue
        except anthropic.APIConnectionError:
            print("\n[Сетевая ошибка. Проверьте подключение.]")
            messages.pop()
            continue

        assistant_text = next(
            (b.text for b in response.content if b.type == "text"), ""
        )
        messages.append({"role": "assistant", "content": assistant_text})

        if is_exit(user_input):
            return


def main():
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if api_key:
        run_online(api_key)
    else:
        run_offline()


if __name__ == "__main__":
    main()
