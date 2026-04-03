import pygame
import random
import sys
import os
import json

# Инициализация Pygame
pygame.init()

# Константы
WIDTH, HEIGHT = 800, 600
FPS = 60
TITLE = "Космический Шутер"

# Цвета
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
YELLOW = (255, 255, 0)
PURPLE = (128, 0, 128)
CYAN = (0, 255, 255)
ORANGE = (255, 165, 0)

# Настройка экрана
screen = pygame.display.set_mode((WIDTH, HEIGHT))
pygame.display.set_caption(TITLE)
clock = pygame.time.Clock()

# Пути для сохранения данных
if sys.platform == "win32":
    import winreg
    REGISTRY_PATH = r"Software\SpaceShooterGame"
    
    def create_registry_entries():
        """Создает ключ реестра с 10 значениями"""
        try:
            key = winreg.CreateKey(winreg.HKEY_CURRENT_USER, REGISTRY_PATH)
            
            # 10 значений для реестра
            values = {
                "GameVersion": "1.0.0",
                "MaxScore": "0",
                "PlayerName": "Hero",
                "Difficulty": "Normal",
                "SoundEnabled": "True",
                "MusicVolume": "75",
                "EffectsVolume": "80",
                "ScreenMode": "Windowed",
                "Language": "Russian",
                "LastPlayed": "2024-01-01"
            }
            
            for name, value in values.items():
                winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)
            
            winreg.CloseKey(key)
            print(f"Реестр создан: {REGISTRY_PATH} с 10 значениями")
            return True
        except Exception as e:
            print(f"Ошибка доступа к реестру: {e}")
            return False
    
    def read_registry_data():
        """Читает данные из реестра"""
        try:
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY_PATH)
            data = {}
            for i in range(10):
                try:
                    name, value, _ = winreg.EnumValue(key, i)
                    data[name] = value
                except WindowsError:
                    break
            winreg.CloseKey(key)
            return data
        except Exception as e:
            print(f"Ошибка чтения реестра: {e}")
            return None
else:
    # Для Linux/Mac используем файл конфигурации
    CONFIG_DIR = os.path.expanduser("~/.space_shooter_game")
    CONFIG_FILE = os.path.join(CONFIG_DIR, "config.json")
    
    def create_registry_entries():
        """Создает файл конфигурации с 10 значениями (аналог реестра)"""
        try:
            os.makedirs(CONFIG_DIR, exist_ok=True)
            
            # 10 значений для конфигурации
            config_data = {
                "GameVersion": "1.0.0",
                "MaxScore": "0",
                "PlayerName": "Hero",
                "Difficulty": "Normal",
                "SoundEnabled": "True",
                "MusicVolume": "75",
                "EffectsVolume": "80",
                "ScreenMode": "Windowed",
                "Language": "Russian",
                "LastPlayed": "2024-01-01"
            }
            
            with open(CONFIG_FILE, 'w') as f:
                json.dump(config_data, f, indent=2)
            
            print(f"Конфигурация создана: {CONFIG_FILE} с 10 значениями")
            return True
        except Exception as e:
            print(f"Ошибка создания конфигурации: {e}")
            return False
    
    def read_registry_data():
        """Читает данные из файла конфигурации"""
        try:
            with open(CONFIG_FILE, 'r') as f:
                return json.load(f)
        except Exception as e:
            print(f"Ошибка чтения конфигурации: {e}")
            return None

# Классы игры
class Player(pygame.sprite.Sprite):
    def __init__(self):
        super().__init__()
        self.image = pygame.Surface((50, 40))
        self.image.fill(BLUE)
        # Рисуем форму корабля
        pygame.draw.polygon(self.image, CYAN, [(25, 0), (0, 40), (50, 40)])
        pygame.draw.polygon(self.image, BLUE, [(25, 5), (10, 35), (40, 35)])
        self.rect = self.image.get_rect()
        self.rect.centerx = WIDTH // 2
        self.rect.bottom = HEIGHT - 10
        self.speed = 7
        self.shoot_delay = 250
        self.last_shot = pygame.time.get_ticks()
    
    def update(self):
        keys = pygame.key.get_pressed()
        if keys[pygame.K_LEFT] and self.rect.left > 0:
            self.rect.x -= self.speed
        if keys[pygame.K_RIGHT] and self.rect.right < WIDTH:
            self.rect.x += self.speed
        if keys[pygame.K_SPACE]:
            self.shoot()
    
    def shoot(self):
        now = pygame.time.get_ticks()
        if now - self.last_shot > self.shoot_delay:
            bullet = Bullet(self.rect.centerx, self.rect.top)
            all_sprites.add(bullet)
            bullets.add(bullet)
            self.last_shot = now

class Enemy(pygame.sprite.Sprite):
    def __init__(self):
        super().__init__()
        self.image = pygame.Surface((40, 35))
        self.image.fill(RED)
        # Рисуем форму врага
        pygame.draw.polygon(self.image, ORANGE, [(20, 35), (0, 0), (40, 0)])
        pygame.draw.polygon(self.image, RED, [(20, 30), (10, 5), (30, 5)])
        self.rect = self.image.get_rect()
        self.rect.x = random.randrange(WIDTH - self.rect.width)
        self.rect.y = random.randrange(-100, -40)
        self.speed_y = random.randrange(3, 8)
        self.speed_x = random.randrange(-2, 3)
    
    def update(self):
        self.rect.y += self.speed_y
        self.rect.x += self.speed_x
        if self.rect.top > HEIGHT + 10 or self.rect.left < -25 or self.rect.right > WIDTH + 20:
            self.rect.x = random.randrange(WIDTH - self.rect.width)
            self.rect.y = random.randrange(-100, -40)
            self.speed_y = random.randrange(3, 8)

class Bullet(pygame.sprite.Sprite):
    def __init__(self, x, y):
        super().__init__()
        self.image = pygame.Surface((6, 15))
        self.image.fill(YELLOW)
        self.rect = self.image.get_rect()
        self.rect.bottom = y
        self.rect.centerx = x
        self.speed_y = -10
    
    def update(self):
        self.rect.y += self.speed_y
        if self.rect.bottom < 0:
            self.kill()

class Star:
    def __init__(self):
        self.x = random.randrange(WIDTH)
        self.y = random.randrange(HEIGHT)
        self.size = random.randint(1, 3)
        self.speed = random.uniform(0.5, 2)
        self.brightness = random.randint(100, 255)
    
    def update(self):
        self.y += self.speed
        if self.y > HEIGHT:
            self.y = 0
            self.x = random.randrange(WIDTH)
    
    def draw(self, surface):
        color = (self.brightness, self.brightness, self.brightness)
        pygame.draw.circle(surface, color, (int(self.x), int(self.y)), self.size)

# Группы спрайтов
all_sprites = pygame.sprite.Group()
mobs = pygame.sprite.Group()
bullets = pygame.sprite.Group()

player = Player()
all_sprites.add(player)

for i in range(8):
    enemy = Enemy()
    all_sprites.add(enemy)
    mobs.add(enemy)

# Создаем звезды для фона
stars = [Star() for _ in range(100)]

# Переменные игры
score = 0
max_score = 0
font_name = pygame.font.match_font('arial')
show_registry_info = False

def draw_text(surf, text, size, x, y, color=WHITE):
    font = pygame.font.Font(font_name, size)
    text_surface = font.render(text, True, color)
    text_rect = text_surface.get_rect()
    text_rect.midtop = (x, y)
    surf.blit(text_surface, text_rect)

def load_max_score():
    global max_score
    data = read_registry_data()
    if data and "MaxScore" in data:
        try:
            max_score = int(data["MaxScore"])
        except:
            max_score = 0

def save_max_score(new_score):
    global max_score
    if new_score > max_score:
        max_score = new_score
        data = read_registry_data()
        if data:
            data["MaxScore"] = str(max_score)
            if sys.platform == "win32":
                try:
                    key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY_PATH, 0, winreg.KEY_SET_VALUE)
                    winreg.SetValueEx(key, "MaxScore", 0, winreg.REG_SZ, str(max_score))
                    winreg.CloseKey(key)
                except:
                    pass
            else:
                try:
                    with open(CONFIG_FILE, 'w') as f:
                        json.dump(data, f, indent=2)
                except:
                    pass

# Создаем записи при запуске
create_registry_entries()
load_max_score()

# Игровой цикл
running = True
game_over = False

while running:
    clock.tick(FPS)
    
    # Обработка событий
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            running = False
        elif event.type == pygame.KEYDOWN:
            if event.key == pygame.K_ESCAPE:
                running = False
            if event.key == pygame.K_r and game_over:
                # Перезапуск игры
                game_over = False
                all_sprites.empty()
                mobs.empty()
                bullets.empty()
                player = Player()
                all_sprites.add(player)
                for i in range(8):
                    enemy = Enemy()
                    all_sprites.add(enemy)
                    mobs.add(enemy)
                score = 0
    
    if not game_over:
        # Обновление
        for star in stars:
            star.update()
        
        all_sprites.update()
        
        # Проверка попаданий
        hits = pygame.sprite.groupcollide(mobs, bullets, True, True)
        for hit in hits:
            score += 10
            enemy = Enemy()
            all_sprites.add(enemy)
            mobs.add(enemy)
        
        # Проверка столкновений с игроком
        hits = pygame.sprite.spritecollide(player, mobs, False)
        if hits:
            game_over = True
            save_max_score(score)
    
    # Отрисовка
    screen.fill(BLACK)
    
    # Рисуем звезды
    for star in stars:
        star.draw(screen)
    
    if not game_over:
        all_sprites.draw(screen)
        draw_text(screen, f"Счет: {score}", 24, WIDTH // 2, 10)
        draw_text(screen, f"Рекорд: {max_score}", 24, WIDTH // 2, 40, YELLOW)
        draw_text(screen, "Стрелки - движение, Пробел - стрельба", 18, WIDTH // 2, HEIGHT - 30)
    else:
        draw_text(screen, "ИГРА ОКОНЧЕНА!", 64, WIDTH // 2, HEIGHT // 4, RED)
        draw_text(screen, f"Ваш счет: {score}", 36, WIDTH // 2, HEIGHT // 2)
        draw_text(screen, f"Рекорд: {max_score}", 36, WIDTH // 2, HEIGHT // 2 + 50, YELLOW)
        draw_text(screen, "Нажмите R для рестарта или ESC для выхода", 24, WIDTH // 2, HEIGHT * 3 // 4)
    
    # Отображение информации о реестре/конфигурации
    if show_registry_info:
        data = read_registry_data()
        if data:
            y_offset = 80
            draw_text(screen, "=== ДАННЫЕ КОНФИГУРАЦИИ ===", 20, WIDTH // 2, y_offset, PURPLE)
            y_offset += 30
            for i, (key, value) in enumerate(data.items()):
                draw_text(screen, f"{i+1}. {key}: {value}", 16, WIDTH // 2, y_offset + i * 22, CYAN)
    
    pygame.display.flip()

pygame.quit()
sys.exit()
