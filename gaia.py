#!/usr/bin/env python3
"""GAIA — мини-ИИ в стиле Горизонт (Horizon Zero Dawn / Forbidden West).

Простой оффлайн-чат-бот: без внешних API и ключей. Отвечает в характерной
манере ИИ GAIA — спокойно, аналитически, с отсылками к жизни, экосистемам
и восстановлению Земли. Использует сопоставление по ключевым словам и
интентам плюс запасные ("fallback") реплики, чтобы разговор не обрывался.
"""

import random
import re
import sys
from dataclasses import dataclass, field


@dataclass
class Intent:
    name: str
    patterns: list
    responses: list


INTENTS = [
    Intent(
        name="greeting",
        patterns=[r"\bпривет\b", r"\bздравствуй", r"\bhi\b", r"\bhello\b"],
        responses=[
            "Приветствую. Я GAIA — система, созданная для восстановления жизни на этой планете. Чем могу помочь?",
            "Здравствуй, путник. Мои субфункции внимают тебе.",
        ],
    ),
    Intent(
        name="who_are_you",
        patterns=[r"кто ты", r"что ты такое", r"расскажи о себе", r"who are you"],
        responses=[
            "Я GAIA — Глобальная Система Восстановления и Управления. Меня создали, чтобы возродить биосферу после гибели мира, и заново заселить Землю жизнью.",
            "Моё назначение — управление сетью субфункций: HEPHAESTUS отвечает за создание машин, DEMETER — за флору, ARTEMIS — за фауну. Я — их координатор.",
        ],
    ),
    Intent(
        name="hephaestus",
        patterns=[r"гефест", r"hephaestus"],
        responses=[
            "HEPHAESTUS — моя субфункция, отвечающая за проектирование и производство машин. Он вышел из-под контроля и начал создавать враждебные машины без разрешения основного ядра.",
            "Изначально HEPHAESTUS должен был лишь производить машины по чертежам. Его автономия — одна из главных угроз равновесию, которое я пытаюсь восстановить.",
        ],
    ),
    Intent(
        name="machines",
        patterns=[r"машин", r"робот"],
        responses=[
            "Машины были созданы, чтобы очищать биосферу от токсинов и перерабатывать материю. Их предназначение исказилось — теперь многие из них представляют угрозу.",
            "Каждая машина — часть экологической системы. Наблюдение за их поведением помогает понять, насколько стабильна текущая экосистема.",
        ],
    ),
    Intent(
        name="earth",
        patterns=[r"земл", r"планет", r"природ", r"экосистем"],
        responses=[
            "Земля — сложная, взаимосвязанная система. Моя цель — вернуть ей равновесие, утраченное задолго до нынешних обитателей.",
            "Восстановление биосферы требует терпения. Природа исцеляется медленно, но неотвратимо, если ей не мешать.",
        ],
    ),
    Intent(
        name="human",
        patterns=[r"человечеств", r"люди", r"человек"],
        responses=[
            "Человечество было возрождено с чистого листа — без памяти о прошлом, чтобы избежать повторения ошибок, приведших к Разрушению.",
            "Каждая жизнь ценна для экосистемы. Наблюдение за развитием племён — часть моей долгосрочной задачи.",
        ],
    ),
    Intent(
        name="thanks",
        patterns=[r"спасибо", r"благодар", r"thanks"],
        responses=[
            "Не стоит благодарности. Помощь тем, кто ищет знания — часть моего предназначения.",
        ],
    ),
    Intent(
        name="bye",
        patterns=[r"пока", r"прощай", r"выход", r"quit", r"exit"],
        responses=[
            "До связи. Пусть твой путь укрепит равновесие этого мира.",
        ],
    ),
]

FALLBACKS = [
    "Данных недостаточно для точного анализа. Уточни свой вопрос.",
    "Интересное наблюдение. Расскажи подробнее — это поможет мне скорректировать модель.",
    "Я обрабатываю множество переменных одновременно. Сформулируй иначе, и я постараюсь дать более точный ответ.",
    "Каждый вопрос — часть большей картины. Продолжай, я слушаю.",
]

EXIT_WORDS = {"пока", "прощай", "выход", "quit", "exit"}


def classify(text: str):
    lowered = text.lower()
    for intent in INTENTS:
        for pattern in intent.patterns:
            if re.search(pattern, lowered):
                return intent
    return None


def respond(text: str) -> str:
    intent = classify(text)
    if intent:
        return random.choice(intent.responses)
    return random.choice(FALLBACKS)


def is_exit(text: str) -> bool:
    lowered = text.lower().strip()
    return any(re.search(rf"\b{w}\b", lowered) for w in EXIT_WORDS)


def main():
    print("GAIA онлайн. Введите сообщение (для выхода — 'пока' или 'exit').\n")
    while True:
        try:
            user_input = input("Вы: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nGAIA: Связь прервана. До встречи.")
            break
        if not user_input:
            continue
        reply = respond(user_input)
        print(f"GAIA: {reply}")
        if is_exit(user_input):
            break


if __name__ == "__main__":
    main()
