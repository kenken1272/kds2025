// プリンタ用UART実体定義
// 他の翻訳単位 (main.cpp など) で extern HardwareSerial printerSerial; 参照あり
// 以前は printer_render.cpp に置かれていたが分離して単一定義とする
#include <Arduino.h>

// UART2 をプリンタ用に使用
HardwareSerial printerSerial(2);
