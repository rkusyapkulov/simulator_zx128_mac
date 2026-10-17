#import "ViewController.h"
#import "simulator_zx128.h"
#import <Carbon/Carbon.h>
#import <AudioToolbox/AudioToolbox.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// Dimensions of the ZX Spectrum screen layout with borders included
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 1024

// Ссылки на внешние функции ядра C
extern void zx128_init(const char *rom1, const char *rom2, const char *trdos);
extern void zx128_run_frame(void);
extern const uint32_t* zx128_get_pixel_buffer(void);
extern void SetPhysicalMatrixKey(int idx, bool down);
extern bool zx128_load_tape(const char *filepath);
extern bool zx128_load_z80(const char *filepath);
extern bool zx128_load_sna(const char *filepath);
extern void zx128_reset(void);
extern void zx128_get_audio_samples(int16_t *buffer, int sample_count);
extern void ClearAudioBuffers(void);

@interface ViewController () {
    AudioQueueRef audioQueue;
    AudioQueueBufferRef audioBuffers[2];
}
@end

@implementation ViewController

// callback функция для AudioQueue (вызывается системой, когда ей нужны новые аудио-данные)
static void AudioQueueCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    int sampleCount = inBuffer->mAudioDataBytesCapacity / sizeof(int16_t);
    zx128_get_audio_samples((int16_t *)inBuffer->mAudioData, sampleCount);
    inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

- (void)viewDidLoad {
    [super viewDidLoad];
    
    // Настройка слоя для рендеринга
    self.view.wantsLayer = YES;
    self.emulationLayer = [CALayer layer];
    self.emulationLayer.frame = self.view.bounds;
    self.emulationLayer.contentsGravity = kCAGravityResizeAspect;
    
    // Отключаем интерполяцию для четких пикселей (Pixel Art)
    self.emulationLayer.magnificationFilter = kCAFilterNearest;
    [self.view.layer addSublayer:self.emulationLayer];
    
    // Locate the unified ROM image (ZXSpectrum128_0.rom + ZXSpectrum128_1.rom + TRDOS.rom) inside the app bundle
    NSString *rom1 = [[NSBundle mainBundle] pathForResource:@"ZXSpectrum128_0" ofType:@"rom"];
    if (!rom1) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Критическая ошибка запуска"];
        [alert setInformativeText:@"Файл 'ZXSpectrum128_0.rom' не найден в ресурсах приложения.\nПожалуйста, добавьте дамп ПЗУ 'ZXSpectrum128_0.rom' в Copy Bundle Resources."];
        [alert setAlertStyle:NSAlertStyleCritical];
        [alert runModal];
        
        // Безопасное асинхронное закрытие приложения без крэша AppKit
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
        return;
    }
    NSString *rom2 = [[NSBundle mainBundle] pathForResource:@"ZXSpectrum128_1" ofType:@"rom"];
    if (!rom2) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Критическая ошибка запуска"];
        [alert setInformativeText:@"Файл 'ZXSpectrum128_1.rom' не найден в ресурсах приложения.\nПожалуйста, добавьте дамп ПЗУ 'ZXSpectrum128_1.rom' в Copy Bundle Resources."];
        [alert setAlertStyle:NSAlertStyleCritical];
        [alert runModal];
        
        // Безопасное асинхронное закрытие приложения без крэша AppKit
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
        return;
    }
    NSString *trdos = [[NSBundle mainBundle] pathForResource:@"TRDOS" ofType:@"rom"];
    if (!trdos) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Критическая ошибка запуска"];
        [alert setInformativeText:@"Файл 'TRDOS.rom' не найден в ресурсах приложения.\nПожалуйста, добавьте дамп ПЗУ 'TRDOS.rom' в Copy Bundle Resources."];
        [alert setAlertStyle:NSAlertStyleCritical];
        [alert runModal];
        
        // Безопасное асинхронное закрытие приложения без крэша AppKit
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
        return;
    }
    // Инициализация C-ядра
    zx128_init([rom1 UTF8String], [rom2 UTF8String], [trdos UTF8String]);
    [self initAudioEngine];
    
    // Запуск цикла 50 Гц
    [self startEmulation];
}

- (void)initAudioEngine {
    AudioStreamBasicDescription format;
    format.mSampleRate = AUDIO_SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = 2; // Моно 16-бит = 2 байта
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2;
    format.mChannelsPerFrame = 1; // Устанавливаем МОНО
    format.mBitsPerChannel = 16;
    format.mReserved = 0;
    
    AudioQueueNewOutput(&format, AudioQueueCallback, (__bridge void *)(self), NULL, NULL, 0, &audioQueue);
    
    for (int i = 0; i < 2; i++) {
        // Выделяем буфер под моно-сэмплы
        AudioQueueAllocateBuffer(audioQueue, AUDIO_BUFFER_SIZE * sizeof(int16_t), &audioBuffers[i]);
        AudioQueueCallback((__bridge void *)(self), audioQueue, audioBuffers[i]);
    }
    AudioQueueStart(audioQueue, NULL);
}

- (void)dealloc {
    AudioQueueStop(audioQueue, YES);
    AudioQueueDispose(audioQueue, YES);
    [self stopEmulation];
}

- (void)viewDidLayout {
    [super viewDidLayout];
    self.emulationLayer.frame = self.view.bounds;
}

- (void)startEmulation {
    [self stopEmulation];
    
    // Запуск цикла 50 Гц
    self.emulationTimer = [NSTimer scheduledTimerWithTimeInterval:0.02
                                                           target:self
                                                         selector:@selector(emulationLoop)
                                                         userInfo:nil
                                                          repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:self.emulationTimer forMode:NSRunLoopCommonModes];
}

- (void)stopEmulation {
    if (self.emulationTimer) {
        [self.emulationTimer invalidate];
        self.emulationTimer = nil;
    }
}

- (void)emulationLoop {
    // 1. Выполняем симуляцию одного кадра (69888 тактов CPU)
    zx128_run_frame();
 
    // 2. Получаем RGBA буфер экрана и обновляем UI
    const uint32_t *buffer = zx128_get_pixel_buffer();
    [self updateEmulationViewWithPixelBuffer:buffer];
}

- (void)updateEmulationViewWithPixelBuffer:(const uint32_t *)pixelBuffer {
    if (!pixelBuffer) return;

    size_t bufferSize = SCREEN_WIDTH * SCREEN_HEIGHT * 4;
    
    // Создаем копию данных для безопасной передачи CoreGraphics
    CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)pixelBuffer, bufferSize);
    if (!dataRef) return;

    CGDataProviderRef provider = CGDataProviderCreateWithCFData(dataRef);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    
    CGImageRef imageRef = CGImageCreate(
        SCREEN_WIDTH, SCREEN_HEIGHT,
        8,                        // bits per component
        32,                       // bits per pixel
        SCREEN_WIDTH * 4,         // bytes per row
        colorSpace,
        // Соответствует формату 0xRRGGBBAA в памяти на Mac
        kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big,
        provider, NULL, NO, kCGRenderingIntentDefault
    );
    
    // Обновляем содержимое слоя в основном потоке с правильным ARC-модификатором
    if (imageRef) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self.emulationLayer.contents = (__bridge id _Nullable)(imageRef);
        [CATransaction commit];
        CGImageRelease(imageRef);
    }
    
    // Освобождаем ресурсы CoreFoundation / CoreGraphics
    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    CFRelease(dataRef);
}

// Вспомогательный метод для маппинга клавиш macOS на матрицу ZX Spectrum
- (void)handleKeyEvent:(NSEvent *)event isPressed:(BOOL)isPressed {
    unsigned short keyCode = [event keyCode];
    int targetIndex = -1;
    
    // СИНХРОННАЯ ОБРАБОТКА BACKSPACE (Caps Shift [30] + '0')
    if (keyCode == kVK_Delete) {
        SetPhysicalMatrixKey(30, isPressed); // Caps Shift
        SetPhysicalMatrixKey(9, isPressed);  // '0' (клавиша DEL на Спектруме)
        return;
    }
    
    // СИНХРОННАЯ ОБРАБОТКА СТРЕЛОК НАВИГАЦИИ (Caps Shift [30] + 5/6/7/8)
    if (keyCode == kVK_LeftArrow) {
        SetPhysicalMatrixKey(30, isPressed); // Caps Shift
        SetPhysicalMatrixKey(4, isPressed);  // '5' (влево)
        return;
    }
    if (keyCode == kVK_DownArrow) {
        SetPhysicalMatrixKey(30, isPressed); // Caps Shift
        SetPhysicalMatrixKey(5, isPressed);  // '6' (вниз)
        return;
    }
    if (keyCode == kVK_UpArrow) {
        SetPhysicalMatrixKey(30, isPressed); // Caps Shift
        SetPhysicalMatrixKey(6, isPressed);  // '7' (вверх)
        return;
    }
    if (keyCode == kVK_RightArrow) {
        SetPhysicalMatrixKey(30, isPressed); // Caps Shift
        SetPhysicalMatrixKey(7, isPressed);  // '8' (вправо)
        return;
    }
    
    // Маппинг стандартных клавиш Mac на сквозной индекс матрицы Спектрума (0..39)
    switch (keyCode) {
        // Цифровой ряд: 1..0 (Индексы 0..9)
        case kVK_ANSI_1: targetIndex = 0; break;
        case kVK_ANSI_2: targetIndex = 1; break;
        case kVK_ANSI_3: targetIndex = 2; break;
        case kVK_ANSI_4: targetIndex = 3; break;
        case kVK_ANSI_5: targetIndex = 4; break;
        case kVK_ANSI_6: targetIndex = 5; break;
        case kVK_ANSI_7: targetIndex = 6; break;
        case kVK_ANSI_8: targetIndex = 7; break;
        case kVK_ANSI_9: targetIndex = 8; break;
        case kVK_ANSI_0: targetIndex = 9; break;
            
        // Буквенный ряд 1: Q..P (Индексы 10..19)
        case kVK_ANSI_Q: targetIndex = 10; break;
        case kVK_ANSI_W: targetIndex = 11; break;
        case kVK_ANSI_E: targetIndex = 12; break;
        case kVK_ANSI_R: targetIndex = 13; break;
        case kVK_ANSI_T: targetIndex = 14; break;
        case kVK_ANSI_Y: targetIndex = 15; break;
        case kVK_ANSI_U: targetIndex = 16; break;
        case kVK_ANSI_I: targetIndex = 17; break;
        case kVK_ANSI_O: targetIndex = 18; break;
        case kVK_ANSI_P: targetIndex = 19; break;
            
        // Буквенный ряд 2: A..ENTER (Индексы 20..29)
        case kVK_ANSI_A:  targetIndex = 20; break;
        case kVK_ANSI_S:  targetIndex = 21; break;
        case kVK_ANSI_D:  targetIndex = 22; break;
        case kVK_ANSI_F:  targetIndex = 23; break;
        case kVK_ANSI_G:  targetIndex = 24; break;
        case kVK_ANSI_H:  targetIndex = 25; break;
        case kVK_ANSI_J:  targetIndex = 26; break;
        case kVK_ANSI_K:  targetIndex = 27; break;
        case kVK_ANSI_L:  targetIndex = 28; break;
        case kVK_Return:  targetIndex = 29; break;
            
        // Нижний ряд: Z..SPACE (Индексы 31..39, модификаторы обрабатываются в flagsChanged)
        case 0x06:        targetIndex = 31; break; // Клавиша Z
        case kVK_ANSI_X:  targetIndex = 32; break;
        case kVK_ANSI_C:  targetIndex = 33; break;
        case kVK_ANSI_V:  targetIndex = 34; break;
        case kVK_ANSI_B:  targetIndex = 35; break;
        case kVK_ANSI_N:  targetIndex = 36; break;
        case kVK_ANSI_M:  targetIndex = 37; break;
        case kVK_Space:   targetIndex = 39; break;
            
        default: break;
    }
    
    if (targetIndex != -1) {
        SetPhysicalMatrixKey(targetIndex, isPressed);
    }
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    [self handleKeyEvent:event isPressed:YES];
}

- (void)keyUp:(NSEvent *)event {
    [self handleKeyEvent:event isPressed:NO];
}

- (void)flagsChanged:(NSEvent *)event {
    BOOL isShift = ([event modifierFlags] & NSEventModifierFlagShift) != 0;
    BOOL isOption = ([event modifierFlags] & NSEventModifierFlagOption) != 0;

    // Передаем состояние системных переключателей в общую физическую матрицу
    SetPhysicalMatrixKey(30, isShift);   // Сквозной индекс 30 = Caps Shift
    SetPhysicalMatrixKey(38, isOption);  // Сквозной индекс 38 = Symbol Shift
}

- (void)viewDidAppear {
    [super viewDidAppear];
    [self setupMenu];
    [self.view.window makeFirstResponder:self];
}

// --- MACOS MENUBAR POPULATION ---
- (void)setupMenu {
    NSMenu *mainMenu = [NSApp mainMenu];
    NSMenuItem *fileMenuItem = [mainMenu itemWithTitle:@"File"];
    if (!fileMenuItem) {
        fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
        [mainMenu addItem:fileMenuItem];
    }
    NSMenu *fileMenu = [fileMenuItem submenu] ? [fileMenuItem submenu] : [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenuItem setSubmenu:fileMenu];
    
    [fileMenu addItem:[NSMenuItem separatorItem]];
    
    NSMenuItem *openItem = [[NSMenuItem alloc] initWithTitle:@"Open Tape (.tap)..." action:@selector(openTapeDialog:) keyEquivalent:@"t"];
    [openItem setTarget:self];
    [fileMenu addItem:openItem];
    
    // Добавление пункта меню для загрузки формата .Z80
    NSMenuItem *loadZ80Item = [[NSMenuItem alloc] initWithTitle:@"Open Snapshot (.z80)..." action:@selector(openZ80Dialog:) keyEquivalent:@"z"];
    loadZ80Item.target = self;
    [fileMenu addItem:loadZ80Item];
    
    NSMenuItem *loadSnaItem = [[NSMenuItem alloc] initWithTitle:@"Open Snapshot (.sna)..." action:@selector(openSnapshotDialog:) keyEquivalent:@"l"];
    loadSnaItem.target = self;
    [fileMenu addItem:loadSnaItem];

    NSMenuItem *loadTrdItem = [[NSMenuItem alloc] initWithTitle:@"Insert TR-DOS Disk (.trd)..." action:@selector(openTrdDialog:) keyEquivalent:@"d"];
    loadTrdItem.target = self;
    [fileMenu addItem:loadTrdItem];

    NSMenuItem *loadSclItem = [[NSMenuItem alloc] initWithTitle:@"Open Catalog File (.scl)..." action:@selector(openSclDialog:) keyEquivalent:@"s"];
    loadSclItem.target = self;
    [fileMenu addItem:loadSclItem];
    
    [fileMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *resetItem = [[NSMenuItem alloc] initWithTitle:@"Hard Reset" action:@selector(triggerReset:) keyEquivalent:@"r"];
    [resetItem setTarget:self];
    [fileMenu addItem:resetItem];
}

// --- OPENPANEL SELECTION INJECTORS ---
- (void)openTapeDialog:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"tap"]];
    
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseOK && panel.URL) {
            [self stopEmulation];
            ClearAudioBuffers();
            if (self->audioQueue) AudioQueueFlush(self->audioQueue);
            zx128_load_tape([panel.URL.path UTF8String]);
            [self startEmulation];
        }
    }];
}

- (void)openZ80Dialog:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"z80"]];
    
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseOK && panel.URL) {
            [self stopEmulation];
            ClearAudioBuffers();
            if (self->audioQueue) AudioQueueFlush(self->audioQueue);
            zx128_load_z80([panel.URL.path UTF8String]);
            [self startEmulation];
        }
    }];
}

- (void)openSnapshotDialog:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"sna"]];
    
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseOK && panel.URL) {
            [self stopEmulation];
            ClearAudioBuffers();
            if (self->audioQueue) AudioQueueFlush(self->audioQueue);
            zx128_load_sna([panel.URL.path UTF8String]);
            [self startEmulation];
        }
    }];
}

- (void)openTrdDialog:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"trd"]];
    
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseOK && panel.URL) {
            [self stopEmulation];
            ClearAudioBuffers();
            if (self->audioQueue) AudioQueueFlush(self->audioQueue);
            zx128_load_trd([panel.URL.path UTF8String]);
            [self startEmulation];
        }
    }];
}

- (void)openSclDialog:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"scl"]];
    
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseOK && panel.URL) {
            [self stopEmulation];
            ClearAudioBuffers();
            if (self->audioQueue) AudioQueueFlush(self->audioQueue);
            zx128_load_scl([panel.URL.path UTF8String]);
            [self startEmulation];
        }
    }];
}

- (void)triggerReset:(id)sender {
    // 1. Временно останавливаем Cocoa-таймер, чтобы избежать состояния гонки
    [self stopEmulation];
    
    // 2. Вызываем Си-функцию сброса ядра
    zx128_reset();
    // 3. Перезапускаем таймер эмуляции с чистого листа
    [self startEmulation];
    NSLog(@"Emulator reset triggered from application menu.");
}

@end
