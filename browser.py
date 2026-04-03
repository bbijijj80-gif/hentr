#!/usr/bin/env python3
"""
Простой собственный браузер с возможностью поиска и навигации.
Использует tkinter для GUI и webbrowser для открытия страниц.
"""

import tkinter as tk
from tkinter import ttk, messagebox
import webbrowser
import os

class SimpleBrowser:
    def __init__(self, root):
        self.root = root
        self.root.title("Мой Браузер")
        self.root.geometry("1200x800")
        
        # История посещений
        self.history = []
        self.current_url = ""
        
        # Создаем основной фрейм
        self.main_frame = ttk.Frame(root)
        self.main_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        # Панель навигации
        self.nav_frame = ttk.Frame(self.main_frame)
        self.nav_frame.pack(fill=tk.X, pady=(0, 5))
        
        # Кнопки навигации
        self.back_btn = ttk.Button(self.nav_frame, text="← Назад", command=self.go_back)
        self.back_btn.pack(side=tk.LEFT, padx=(0, 5))
        
        self.forward_btn = ttk.Button(self.nav_frame, text="Вперед →", command=self.go_forward)
        self.forward_btn.pack(side=tk.LEFT, padx=(0, 5))
        
        self.refresh_btn = ttk.Button(self.nav_frame, text="↻ Обновить", command=self.refresh)
        self.refresh_btn.pack(side=tk.LEFT, padx=(0, 5))
        
        self.home_btn = ttk.Button(self.nav_frame, text="🏠 Домой", command=self.go_home)
        self.home_btn.pack(side=tk.LEFT, padx=(0, 5))
        
        # Поле ввода URL/поиска
        self.url_var = tk.StringVar()
        self.url_entry = ttk.Entry(self.nav_frame, textvariable=self.url_var, font=("Arial", 12))
        self.url_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 5))
        self.url_entry.bind("<Return>", self.navigate)
        
        # Кнопка перехода
        self.go_btn = ttk.Button(self.nav_frame, text="Перейти", command=self.navigate)
        self.go_btn.pack(side=tk.LEFT)
        
        # Текстовая область для отображения контента (информация о странице)
        self.content_frame = ttk.Frame(self.main_frame)
        self.content_frame.pack(fill=tk.BOTH, expand=True)
        
        self.text_area = tk.Text(self.content_frame, wrap=tk.WORD, font=("Arial", 11))
        self.text_area.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        scrollbar = ttk.Scrollbar(self.content_frame, command=self.text_area.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.text_area.config(yscrollcommand=scrollbar.set)
        
        # Статус бар
        self.status_var = tk.StringVar()
        self.status_var.set("Готов")
        self.status_bar = ttk.Label(root, textvariable=self.status_var, relief=tk.SUNKEN, anchor=tk.W)
        self.status_bar.pack(side=tk.BOTTOM, fill=tk.X)
        
        # Меню
        self.create_menu()
        
        # Загружаем домашнюю страницу
        self.go_home()
    
    def create_menu(self):
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)
        
        # Меню Файл
        file_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="Файл", menu=file_menu)
        file_menu.add_command(label="Новое окно", command=self.new_window)
        file_menu.add_separator()
        file_menu.add_command(label="Выход", command=self.root.quit)
        
        # Меню История
        history_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="История", menu=history_menu)
        self.history_menu_item = history_menu
        history_menu.add_command(label="Показать историю", command=self.show_history)
        
        # Меню Помощь
        help_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="Помощь", menu=help_menu)
        help_menu.add_command(label="О браузере", command=self.show_about)
    
    def is_url(self, text):
        """Проверяет, является ли текст URL"""
        return text.startswith(('http://', 'https://', 'file://'))
    
    def search_or_navigate(self, text):
        """Определяет: поиск или переход по URL"""
        if self.is_url(text):
            return text
        else:
            # Поиск через Google
            search_url = f"https://www.google.com/search?q={text.replace(' ', '+')}"
            return search_url
    
    def navigate(self, event=None):
        """Переход по URL или поиск"""
        url = self.url_var.get().strip()
        if not url:
            return
        
        full_url = self.search_or_navigate(url)
        
        try:
            # Сохраняем в историю
            if self.current_url:
                self.history.append(self.current_url)
            
            self.current_url = full_url
            self.status_var.set(f"Загрузка: {full_url}")
            
            # Открываем в системном браузере
            webbrowser.open(full_url)
            
            # Обновляем отображение
            self.display_page_info(full_url)
            self.status_var.set("Страница загружена")
            
        except Exception as e:
            messagebox.showerror("Ошибка", f"Не удалось загрузить страницу:\n{str(e)}")
            self.status_var.set("Ошибка загрузки")
    
    def display_page_info(self, url):
        """Отображает информацию о странице в текстовой области"""
        self.text_area.delete(1.0, tk.END)
        
        info = f"""
=== ИНФОРМАЦИЯ О СТРАНИЦЕ ===

URL: {url}

Страница была открыта в вашем системном браузере по умолчанию.

Этот браузер использует системный браузер для отображения веб-контента,
но предоставляет собственный интерфейс для навигации и поиска.

Функции:
• Введите URL для перехода на сайт
• Введите текст для поиска в Google
• Используйте кнопки навигации
• История посещений сохраняется

===============================
"""
        self.text_area.insert(tk.END, info)
    
    def go_back(self):
        """Назад в истории"""
        if self.history:
            previous_url = self.history.pop()
            if previous_url:
                self.url_var.set(previous_url)
                self.navigate()
        else:
            messagebox.showinfo("Инфо", "Нет предыдущих страниц в истории")
    
    def go_forward(self):
        """Вперед (заглушка для будущей реализации)"""
        messagebox.showinfo("Инфо", "Функция 'Вперед' будет доступна в следующей версии")
    
    def refresh(self):
        """Обновить текущую страницу"""
        if self.current_url:
            self.navigate()
        else:
            self.go_home()
    
    def go_home(self):
        """Домашняя страница"""
        home_url = "https://www.google.com"
        self.url_var.set(home_url)
        self.navigate()
    
    def new_window(self):
        """Открыть новое окно браузера"""
        new_root = tk.Toplevel(self.root)
        SimpleBrowser(new_root)
    
    def show_history(self):
        """Показать историю посещений"""
        history_window = tk.Toplevel(self.root)
        history_window.title("История посещений")
        history_window.geometry("600x400")
        
        text = tk.Text(history_window, wrap=tk.WORD, font=("Arial", 10))
        text.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        if self.history:
            history_text = "ИСТОРИЯ ПОСЕЩЕНИЙ:\n\n"
            for i, url in enumerate(self.history[-50:], 1):  # Последние 50 записей
                history_text += f"{i}. {url}\n"
            text.insert(tk.END, history_text)
        else:
            text.insert(tk.END, "История пуста")
        
        text.config(state='disabled')
    
    def show_about(self):
        """Показать информацию о браузере"""
        messagebox.showinfo(
            "О браузере",
            "Мой Браузер v1.0\n\n"
            "Простой браузер с функциями:\n"
            "• Поиск в интернете\n"
            "• Навигация по URL\n"
            "• История посещений\n"
            "• Множественные окна\n\n"
            "Создано с помощью Python и Tkinter"
        )


def main():
    root = tk.Tk()
    
    # Установка стиля
    style = ttk.Style()
    style.theme_use('clam')  # Или 'default', 'alt', 'classic'
    
    app = SimpleBrowser(root)
    root.mainloop()


if __name__ == "__main__":
    main()
