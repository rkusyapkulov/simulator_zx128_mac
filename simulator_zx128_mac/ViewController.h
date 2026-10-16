//
//  ViewController.h
//  simulator_zx128_mac
//
//  Created by user on 16.10.26.
//

#import <Cocoa/Cocoa.h>
#import <SpriteKit/SpriteKit.h>
#import <GameplayKit/GameplayKit.h>
#import <AudioToolbox/AudioToolbox.h>

@interface ViewController : NSViewController

// Слой для эффективного вывода RGBA пикселей экрана ZX Spectrum
@property (nonatomic, strong) CALayer *emulationLayer;

// Таймер для симуляции кадровой частоты 50 Гц (PAL)
@property (nonatomic, strong) NSTimer *emulationTimer;

/**
 * Инициализирует таймер эмуляции и слои вывода
 */
- (void)startEmulation;

/**
 * Останавливает таймер эмуляции
 */
- (void)stopEmulation;

/**
 * Обновляет содержимое слоя emulationLayer из предоставленного RGBA буфера
 * @param pixelBuffer Указатель на массив пикселей размером 256 * 192 * 4 байта
 */
- (void)updateEmulationViewWithPixelBuffer:(const uint32_t *)pixelBuffer;

- (void)openTapeDialog:(id)sender;
- (void)openZ80Dialog:(id)sender;
- (void)openSnapshotDialog:(id)sender;

@end

