# Product Requirements Document: ESP32-S3 Wireless HMI Controller

## Introduction/Overview

The ESP32-S3 Wireless HMI Controller is a next-generation, portable human-machine interface system designed specifically for music visualizer control applications. Built around the Waveshare ESP32-S3-Touch-AMOLED-1.75 development board with M5 Unit Scroll integration, this controller provides ultra-low latency wireless communication, multi-modal input capabilities, and an intuitive circular user interface.

This system addresses the critical need for responsive, professional-grade wireless control interfaces that can match the performance of traditional wired controllers while providing unprecedented flexibility and user experience enhancements through advanced touch gestures, rotary input, and motion sensing.

## Goals

1. **Ultra-Low Latency Communication**: Achieve sub-5ms end-to-end control latency using ESP-NOW protocol
2. **Intuitive Multi-Modal Interface**: Provide seamless integration of touch, rotary, and motion inputs
3. **Professional-Grade Responsiveness**: Deliver 30-60 FPS UI refresh rates with immediate visual feedback
4. **Portable Operation**: Enable 8+ hours of continuous operation with intelligent power management
5. **Reliable Wireless Communication**: Maintain stable connection within 200m range with error recovery
6. **Modular Architecture**: Create clear separation between HMI responsibilities and audio processing systems

## User Stories

**As a Music Producer**, I want to control visualizer parameters wirelessly so that I can make real-time adjustments during live performances without being tethered to equipment.

**As a VJ/Visual Artist**, I want intuitive gesture controls so that I can make complex parameter changes quickly and naturally during performances.

**As a Live Event Technician**, I want a reliable wireless controller so that I can adjust lighting and visual effects from any position in the venue.

**As a Creative Professional**, I want multi-modal input options so that I can use touch gestures for quick selections and rotary controls for precise parameter adjustments.

**As a System Integrator**, I want a modular controller architecture so that I can easily integrate it with different visualizer systems without complex modifications.

## Functional Requirements

### Core Hardware Interface Requirements

1. **ESP32-S3-Touch-AMOLED-1.75 Integration**
   - Initialize and configure 466×466 AMOLED display with CO5300 driver
   - Implement CST9217 capacitive touch controller with multi-touch support
   - Configure QMI8658 6-axis IMU for motion sensing
   - Integrate AXP2101 power management with battery monitoring

2. **M5 Unit Scroll Integration**
   - Establish I2C communication on address 0x40
   - Implement rotary encoder value reading with 12 pulses/revolution resolution
   - Configure integrated button detection
   - Control WS2812C RGB LED for visual feedback

### User Interface Requirements

3. **Circular UI Implementation**
   - Design LVGL-based circular interface optimized for 466×466 display
   - Implement radial menu systems with concentric control rings
   - Create gesture recognition zones (center, cardinal directions, circular swipes)
   - Provide visual feedback for all touch and rotary interactions

4. **Multi-Touch Gesture Recognition**
   - Support tap, swipe (up/down/left/right), pinch, and rotate gestures
   - Implement predictive touch tracking for smooth interactions
   - Enable multi-finger gestures for advanced control patterns
   - Provide haptic-like visual feedback for gesture confirmation

5. **Rotary Control Integration**
   - Map rotary encoder to context-sensitive parameter control
   - Implement push-button for mode switching and confirmations
   - Synchronize RGB LED status with current control mode
   - Enable precision adjustment overlay on touch interface

### Wireless Communication Requirements

6. **ESP-NOW Protocol Implementation**
   - Establish ultra-low latency device-to-device communication
   - Implement structured data packets with touch, rotary, and IMU data
   - Add AES encryption for secure communication
   - Include timestamp synchronization for latency measurement

7. **Data Structure Optimization**
   - Create compact 36-byte data packets within 250-byte ESP-NOW limit
   - Include controller ID, touch coordinates, gesture type, encoder values
   - Add 6-axis IMU data (accelerometer + gyroscope)
   - Include battery level and timestamp for system monitoring

8. **Connection Management**
   - Implement automatic peer discovery and pairing
   - Add connection status monitoring with visual indicators
   - Provide reconnection algorithms with exponential backoff
   - Include signal strength monitoring and adaptive transmission

### Motion Sensing Requirements

9. **IMU Integration and Sensor Fusion**
   - Read QMI8658 IMU data at 100Hz for responsive motion tracking
   - Implement tilt-to-steer functionality with configurable sensitivity
   - Add shake gesture detection for special commands
   - Provide orientation-based UI rotation with smooth transitions

10. **Motion-Enhanced Control**
    - Enable tilt compensation for stable touch detection during movement
    - Implement motion-based parameter modulation
    - Add gesture patterns combining touch and motion inputs
    - Provide motion-triggered mode switching

### Power Management Requirements

11. **Battery Life Optimization**
    - Implement AMOLED-specific power optimizations (dark themes, black backgrounds)
    - Add dynamic power scaling based on battery level
    - Create intelligent sleep modes with instant wake capability
    - Monitor battery voltage and current consumption in real-time

12. **Charging and Power Monitoring**
    - Provide accurate battery level indication (0-100%)
    - Implement safe charging algorithms with temperature monitoring
    - Add low-battery warnings and graceful shutdown procedures
    - Enable power-on self-test and system health monitoring

### Performance Requirements

13. **Real-Time Performance**
    - Achieve 30-60 FPS UI refresh rates with LVGL optimization
    - Maintain 50Hz touch processing for gesture recognition
    - Process sensor data at 100Hz with minimal latency
    - Implement dual-buffering for smooth animations

14. **Latency Optimization**
    - Target <5ms end-to-end communication latency
    - Minimize touch-to-transmission delay to <2ms
    - Optimize sensor fusion algorithms for real-time response
    - Implement predictive algorithms for improved responsiveness

## Non-Goals (Out of Scope)

1. **Audio Processing**: This controller does NOT handle audio analysis, FFT processing, or sound generation
2. **Visual Rendering**: No responsibility for actual visualizer graphics or LED pattern generation  
3. **Network Infrastructure**: No WiFi router management or network configuration tools
4. **Multi-Device Coordination**: Focus on single controller to single receiver communication
5. **Advanced AI/ML**: No machine learning features in initial implementation
6. **External Sensor Integration**: Limited to onboard sensors, no additional sensor expansion
7. **File System Management**: No complex file operations beyond basic configuration storage
8. **Web Interface**: No web-based configuration or control panels

## Design Considerations

### Circular UI Design Patterns
- Leverage psychological advantages of circular interfaces (15% faster processing)
- Implement natural finger movement patterns around display center
- Use concentric rings for hierarchical control structures
- Provide clear visual boundaries for gesture zones

### Hardware Integration Strategy
- Maintain separation between touch processing and wireless communication threads
- Implement interrupt-driven sensor reading for optimal performance
- Use dedicated I2C bus management for reliable multi-device communication
- Design modular hardware abstraction layer for future expansion

### User Experience Philosophy
- Prioritize immediate visual feedback for all user interactions
- Implement progressive disclosure to prevent interface complexity
- Provide consistent interaction patterns across all control modes
- Design for one-handed operation with thumb-based interaction

## Technical Considerations

### Development Framework
- **Primary Platform**: Arduino IDE for rapid prototyping and library ecosystem
- **Alternative**: ESP-IDF for maximum performance optimization in production
- **UI Framework**: LVGL v8+ with circular display optimizations
- **Communication**: ESP-NOW with custom protocol structure

### Hardware Dependencies
- ESP32-S3R8 microcontroller with 16MB Flash, 8MB PSRAM
- 1.75" AMOLED 466×466 display with QSPI interface
- CST9217 capacitive touch controller
- QMI8658 6-axis IMU sensor
- M5 Unit Scroll with STM32F030 controller
- AXP2101 power management IC

### Performance Targets
- **Latency**: <5ms end-to-end communication
- **UI Refresh**: 30-60 FPS sustained
- **Battery Life**: 8+ hours continuous operation
- **Wireless Range**: 200m line-of-sight
- **Touch Response**: <50ms gesture recognition

## Success Metrics

### Technical Performance Metrics
1. **Communication Latency**: <5ms average, <10ms maximum
2. **UI Responsiveness**: 30+ FPS sustained during active use
3. **Battery Life**: >8 hours at 50% display brightness
4. **Connection Reliability**: >99% uptime within 100m range
5. **Touch Accuracy**: <2mm deviation from intended target
6. **Gesture Recognition**: >95% accuracy for trained gestures

### User Experience Metrics
1. **Setup Time**: <2 minutes from power-on to operational
2. **Learning Curve**: <30 minutes to master basic operations
3. **Error Rate**: <5% unintended actions during normal use
4. **Response Satisfaction**: Sub-perceptible delay for all interactions
5. **Reliability**: Zero system crashes during 8-hour operation sessions

### Development Metrics
1. **Code Coverage**: >80% test coverage for critical functions
2. **Build Time**: <30 seconds for full compilation
3. **Flash Usage**: <80% of available 16MB storage
4. **RAM Usage**: <70% of available PSRAM during peak operation

## Open Questions

1. **Haptic Feedback**: Should we implement audio-based haptic feedback through ES8311 codec?
2. **Voice Commands**: Integration level for dual microphone voice control features?
3. **Configuration Storage**: Local vs cloud-based settings synchronization approach?
4. **Multi-Controller Support**: Future scalability for multiple controller coordination?
5. **Calibration Procedures**: Automated vs manual touch and IMU calibration processes?
6. **Update Mechanism**: OTA update strategy for firmware deployment in production?
7. **Error Recovery**: Automatic vs manual recovery procedures for communication failures?
8. **Security Level**: Additional encryption beyond ESP-NOW AES for sensitive applications?

## Implementation Priority

### Phase 1: Core Foundation (Weeks 1-3)
- Basic hardware initialization and testing
- Fundamental LVGL circular UI implementation
- ESP-NOW communication establishment
- Basic touch and rotary input processing

### Phase 2: Advanced Features (Weeks 4-6)
- Multi-touch gesture recognition
- IMU integration and motion controls
- Power management optimization
- Advanced UI animations and feedback

### Phase 3: Integration & Polish (Weeks 7-8)
- End-to-end latency optimization
- Comprehensive error handling
- User experience refinement
- Performance validation and testing

This PRD provides a comprehensive blueprint for developing a professional-grade wireless HMI controller that leverages the unique capabilities of the ESP32-S3-Touch-AMOLED-1.75 platform while maintaining clear architectural boundaries and focusing on responsive user interaction rather than audio processing responsibilities.