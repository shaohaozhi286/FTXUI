// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include "ftxui/component/app.hpp"
#include <algorithm>  // for copy, max, min
#include <array>      // for array
#include <atomic>
#include <chrono>  // for operator-, milliseconds, operator>=, duration, common_type<>::type, time_point
#include <csignal>  // for signal, SIGTSTP, SIGABRT, SIGWINCH, raise, SIGFPE, SIGILL, SIGINT, SIGSEGV, SIGTERM, __sighandler_t, size_t
#include <cstdint>
#include <cstdio>                    // for fileno, stdin
#include <fstream>
#include <ftxui/component/task.hpp>  // for Task, Closure, AnimationTask
#include <ftxui/screen/screen.hpp>  // for Cell, Screen::Cursor, Screen, Screen::Cursor::Hidden
#include <functional>        // for function
#include <initializer_list>  // for initializer_list
#include <iostream>  // for cout, ostream, operator<<, basic_ostream, endl, flush
#include <memory>
#include <mutex>
#include <sstream>
#include <stack>  // for stack
#include <string>
#include <string_view>
#include <thread>   // for thread, sleep_for
#include <tuple>    // for _Swallow_assign, ignore
#include <utility>  // for move, swap
#include <variant>  // for visit, variant
#include <vector>   // for vector
#include "ftxui/component/animation.hpp"  // for TimePoint, Clock, Duration, Params, RequestAnimationFrame
#include "ftxui/component/captured_mouse.hpp"  // for CapturedMouse, CapturedMouseInterface
#include "ftxui/component/component_base.hpp"  // for ComponentBase
#include "ftxui/component/event.hpp"           // for Event
#include "ftxui/component/loop.hpp"            // for Loop
#include "ftxui/component/task_runner.hpp"
#include "ftxui/component/terminal_input_parser.hpp"  // for TerminalInputParser
#include "ftxui/dom/node.hpp"                         // for Node, Render
#include "ftxui/screen/terminal.hpp"                  // for Dimensions, Size
#include "ftxui/screen/util.hpp"                      // for util::clamp
#include "ftxui/util/autoreset.hpp"                   // for AutoReset

#if defined(_WIN32)
#define DEFINE_CONSOLEV2_PROPERTIES
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef UNICODE
#error Must be compiled in UNICODE mode
#endif
#else
#include <fcntl.h>
#include <sys/select.h>  // for select, FD_ISSET, FD_SET, FD_ZERO, fd_set, timeval
#include <termios.h>  // for tcsetattr, termios, tcgetattr, TCSANOW, cc_t, ECHO, ICANON, VMIN, VTIME
#include <unistd.h>  // for STDIN_FILENO, read
#include <cerrno>
#endif

namespace ftxui {

namespace animation {
void RequestAnimationFrame() {
  auto* screen = App::Active();
  if (screen) {
    screen->RequestAnimationFrame();
  }
}
}  // namespace animation

struct App::Internal {
  // Convert char to Event.
  TerminalInputParser terminal_input_parser;

  task::TaskRunner task_runner;

  // The last time a character was received.
  std::chrono::time_point<std::chrono::steady_clock> last_char_time =
      std::chrono::steady_clock::now();

  // The buffer used to output the screen to the terminal.
  // Unlike for std::vector::clear, the C++ standard does not explicitly require
  // that capacity is unchanged by this function, but existing implementations
  // do not change capacity. This means that they do not release the allocated
  // memory (see also shrink_to_fit).
  std::string output_buffer;

  explicit Internal(std::function<void(Event)> out)
      : terminal_input_parser(std::move(out)) {}
};

namespace {

App* g_active_screen = nullptr;  // NOLINT

std::stack<Closure> on_exit_functions;  // NOLINT

void OnExit() {
  while (!on_exit_functions.empty()) {
    on_exit_functions.top()();
    on_exit_functions.pop();
  }
}

#ifndef ACECODE_TUI_INPUT_TRACE
#define ACECODE_TUI_INPUT_TRACE 0
#endif

#if ACECODE_TUI_INPUT_TRACE
std::mutex& AcecodeTraceMutex() {
  static std::mutex mutex;
  return mutex;
}

const char* MouseButtonName(Mouse::Button button) {
  switch (button) {
    case Mouse::Left:
      return "Left";
    case Mouse::Middle:
      return "Middle";
    case Mouse::Right:
      return "Right";
    case Mouse::None:
      return "None";
    case Mouse::WheelUp:
      return "WheelUp";
    case Mouse::WheelDown:
      return "WheelDown";
    case Mouse::WheelLeft:
      return "WheelLeft";
    case Mouse::WheelRight:
      return "WheelRight";
  }
  return "?";
}

const char* MouseMotionName(Mouse::Motion motion) {
  switch (motion) {
    case Mouse::Released:
      return "Released";
    case Mouse::Pressed:
      return "Pressed";
    case Mouse::Moved:
      return "Moved";
  }
  return "?";
}

bool TraceMouseEvent(const Mouse& mouse) {
  return mouse.button == Mouse::Left &&
         (mouse.motion == Mouse::Pressed || mouse.motion == Mouse::Moved ||
          mouse.motion == Mouse::Released);
}

std::string MouseForTrace(const Mouse& mouse) {
  std::ostringstream out;
  out << "button=" << MouseButtonName(mouse.button)
      << " motion=" << MouseMotionName(mouse.motion) << " x=" << mouse.x
      << " y=" << mouse.y << " shift=" << (mouse.shift ? 1 : 0)
      << " meta=" << (mouse.meta ? 1 : 0)
      << " ctrl=" << (mouse.control ? 1 : 0);
  return out.str();
}

void AcecodeTrace(std::string message) {
  std::lock_guard<std::mutex> lock(AcecodeTraceMutex());
  std::ofstream out("acecode.log", std::ios::app);
  if (!out.is_open()) {
    return;
  }
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  out << ms << " DBG [ftxui-app] " << message << '\n';
}
#endif

#if defined(_WIN32)

#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>

extern "C" {
EMSCRIPTEN_KEEPALIVE
void ftxui_on_resize(int columns, int rows) {
  Terminal::SetFallbackSize({
      columns,
      rows,
  });
  std::raise(SIGWINCH);
}
}

#else  // POSIX (Linux & Mac)

int CheckStdinReady(int fd) {
  timeval tv = {0, 0};  // NOLINT
  fd_set fds;
  FD_ZERO(&fds);                                // NOLINT
  FD_SET(fd, &fds);                             // NOLINT
  select(fd + 1, &fds, nullptr, nullptr, &tv);  // NOLINT
  return FD_ISSET(fd, &fds);                    // NOLINT
}

#endif

std::atomic<int> g_signal_exit_count = 0;  // NOLINT
#if !defined(_WIN32)
std::atomic<int> g_signal_stop_count = 0;    // NOLINT
std::atomic<int> g_signal_resize_count = 0;  // NOLINT
#endif

// Async signal safe function
void RecordSignal(int signal) {
  switch (signal) {
    case SIGABRT:
    case SIGFPE:
    case SIGILL:
    case SIGINT:
    case SIGSEGV:
    case SIGTERM:
      g_signal_exit_count++;
      break;

#if !defined(_WIN32)
    case SIGTSTP:  // NOLINT
      g_signal_stop_count++;
      break;

    case SIGWINCH:  // NOLINT
      g_signal_resize_count++;
      break;
#endif

    default:
      break;
  }
}

void ExecuteSignalHandlers() {
  int signal_exit_count = g_signal_exit_count.exchange(0);
  while (signal_exit_count--) {
    App::Private::Signal(*g_active_screen, SIGABRT);
  }

#if !defined(_WIN32)
  int signal_stop_count = g_signal_stop_count.exchange(0);
  while (signal_stop_count--) {
    App::Private::Signal(*g_active_screen, SIGTSTP);
  }

  int signal_resize_count = g_signal_resize_count.exchange(0);
  while (signal_resize_count--) {
    App::Private::Signal(*g_active_screen, SIGWINCH);
  }
#endif
}

void InstallSignalHandler(int sig) {
  auto old_signal_handler = std::signal(sig, RecordSignal);
  on_exit_functions.emplace(
      [=] { std::ignore = std::signal(sig, old_signal_handler); });
}

// CSI: Control Sequence Introducer
const std::string CSI = "\x1b[";  // NOLINT
                                  //
// DCS: Device Control String
const std::string DCS = "\x1bP";  // NOLINT

// ST: String Terminator
const std::string ST = "\x1b\\";  // NOLINT

// DECRQSS: Request Status String
// DECSCUSR: Set Cursor Style
const std::string DECRQSS_DECSCUSR = DCS + "$q q" + ST;  // NOLINT

// DEC: Digital Equipment Corporation
enum class DECMode : std::uint16_t {
  kLineWrap = 7,
  kCursor = 25,

  kMouseX10 = 9,
  kMouseVt200 = 1000,
  kMouseVt200Highlight = 1001,

  kMouseBtnEventMouse = 1002,
  kMouseAnyEvent = 1003,

  kMouseUtf8 = 1005,
  kMouseSgrExtMode = 1006,
  kMouseUrxvtMode = 1015,
  kMouseSgrPixelsMode = 1016,
  kAlternateScreen = 1049,
};

// Device Status Report (DSR) {
enum class DSRMode : std::uint8_t {
  kCursor = 6,
};

std::string Serialize(const std::vector<DECMode>& parameters) {
  bool first = true;
  std::string out;
  for (const DECMode parameter : parameters) {
    if (!first) {
      out += ";";
    }
    out += std::to_string(int(parameter));
    first = false;
  }
  return out;
}

// DEC Private Mode Set (DECSET)
std::string Set(const std::vector<DECMode>& parameters) {
  return CSI + "?" + Serialize(parameters) + "h";
}

// DEC Private Mode Reset (DECRST)
std::string Reset(const std::vector<DECMode>& parameters) {
  return CSI + "?" + Serialize(parameters) + "l";
}

// Device Status Report (DSR)
std::string DeviceStatusReport(DSRMode ps) {
  return CSI + std::to_string(int(ps)) + "n";
}

class CapturedMouseImpl : public CapturedMouseInterface {
 public:
  explicit CapturedMouseImpl(std::function<void(void)> callback)
      : callback_(std::move(callback)) {}
  ~CapturedMouseImpl() override { callback_(); }
  CapturedMouseImpl(const CapturedMouseImpl&) = delete;
  CapturedMouseImpl(CapturedMouseImpl&&) = delete;
  CapturedMouseImpl& operator=(const CapturedMouseImpl&) = delete;
  CapturedMouseImpl& operator=(CapturedMouseImpl&&) = delete;

 private:
  std::function<void(void)> callback_;
};

}  // namespace

App::App(Dimension dimension, int dimx, int dimy, bool use_alternative_screen)
    : Screen(dimx, dimy),
      dimension_(dimension),
      use_alternative_screen_(use_alternative_screen) {
  internal_ = std::make_unique<Internal>(
      [&](Event event) { PostEvent(std::move(event)); });
}

// static
App App::FixedSize(int dimx, int dimy) {
  return {
      Dimension::Fixed,
      dimx,
      dimy,
      /*use_alternative_screen=*/false,
  };
}

/// Create a App taking the full terminal size. This is using the
/// alternate screen buffer to avoid messing with the terminal content.
/// @note This is the same as `App::FullscreenAlternateScreen()`
// static
App App::Fullscreen() {
  return FullscreenAlternateScreen();
}

/// Create a App taking the full terminal size. The primary screen
/// buffer is being used. It means if the terminal is resized, the previous
/// content might mess up with the terminal content.
// static
App App::FullscreenPrimaryScreen() {
  auto terminal = Terminal::Size();
  return {
      Dimension::Fullscreen,
      terminal.dimx,
      terminal.dimy,
      /*use_alternative_screen=*/false,
  };
}

/// Create a App taking the full terminal size. This is using the
/// alternate screen buffer to avoid messing with the terminal content.
// static
App App::FullscreenAlternateScreen() {
  auto terminal = Terminal::Size();
  return {
      Dimension::Fullscreen,
      terminal.dimx,
      terminal.dimy,
      /*use_alternative_screen=*/true,
  };
}

/// Create a App whose width match the terminal output width and
/// the height matches the component being drawn.
// static
App App::TerminalOutput() {
  auto terminal = Terminal::Size();
  return {
      Dimension::TerminalOutput,
      terminal.dimx,
      terminal.dimy,  // Best guess.
      /*use_alternative_screen=*/false,
  };
}

App::~App() = default;

/// Create a App whose width and height match the component being
/// drawn.
// static
App App::FitComponent() {
  auto terminal = Terminal::Size();
  return {
      Dimension::FitComponent,
      terminal.dimx,  // Best guess.
      terminal.dimy,  // Best guess.
      false,
  };
}

/// @brief Set whether mouse is tracked and events reported.
/// called outside of the main loop. E.g `App::Loop(...)`.
/// @param enable Whether to enable mouse event tracking.
/// @note This muse be called outside of the main loop. E.g. before calling
/// `App::Loop`.
/// @note Mouse tracking is enabled by default.
/// @note Mouse tracking is only supported on terminals that supports it.
///
/// ### Example
///
/// ```cpp
/// auto screen = App::TerminalOutput();
/// screen.TrackMouse(false);
/// screen.Loop(component);
/// ```
void App::TrackMouse(bool enable) {
  track_mouse_ = enable;
}

void App::EnableMouseTracking(bool flush) {
  if (!track_mouse_ || mouse_tracking_enabled_) {
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("EnableMouseTracking skipped track_mouse=" +
                 std::to_string(track_mouse_ ? 1 : 0) +
                 " enabled=" +
                 std::to_string(mouse_tracking_enabled_ ? 1 : 0) +
                 " frame=" + std::to_string(frame_count_) +
                 " cursor=(" + std::to_string(cursor_x_) + "," +
                 std::to_string(cursor_y_) + ")");
#endif
    return;
  }
#if ACECODE_TUI_INPUT_TRACE
  AcecodeTrace("EnableMouseTracking flush=" + std::to_string(flush ? 1 : 0) +
               " frame=" + std::to_string(frame_count_) + " cursor=(" +
               std::to_string(cursor_x_) + "," +
               std::to_string(cursor_y_) + ")");
#endif
  TerminalSend(Set({DECMode::kMouseVt200}));
  // ACECODE-PATCH(idle-mouse-redraw): use button-event tracking instead of
  // any-event tracking. Passive hover motion should not generate events, but
  // clicks, wheel events, and button-held drags still need to be reported.
  TerminalSend(Set({DECMode::kMouseBtnEventMouse}));
  TerminalSend(Set({DECMode::kMouseUrxvtMode}));
  TerminalSend(Set({DECMode::kMouseSgrExtMode}));
  if (flush) {
    TerminalFlush();
  }
  mouse_tracking_enabled_ = true;
}

bool App::IsTerminalOutputPrimaryScreen() const {
  return dimension_ == Dimension::TerminalOutput && !use_alternative_screen_;
}

bool App::CursorPositionIsUsable(int x, int y) const {
  if (!IsTerminalOutputPrimaryScreen()) {
    return true;
  }
  if (x < 1 || y < 1) {
    return false;
  }

  const Dimensions terminal = Terminal::Size();
  if (terminal.dimx > 0 && dimx_ > 0 && x + dimx_ - 1 > terminal.dimx) {
    return false;
  }
  if (terminal.dimy > 0 && dimy_ > 0 && y + dimy_ - 1 > terminal.dimy) {
    return false;
  }
  return true;
}

/// @brief Enable or disable automatic piped input handling.
/// When enabled, FTXUI will detect piped input and redirect stdin from /dev/tty
/// for keyboard input, allowing applications to read piped data while still
/// receiving interactive keyboard events.
/// @param enable Whether to enable piped input handling. Default is true.
/// @note This must be called before Loop().
/// @note This feature is enabled by default.
/// @note This feature is only available on POSIX systems (Linux/macOS).
void App::HandlePipedInput(bool enable) {
  handle_piped_input_ = enable;
}

/// @brief Add a task to the main loop.
/// It will be executed later, after every other scheduled tasks.
void App::Post(Task task) {
  internal_->task_runner.PostTask([this, task = std::move(task)]() mutable {
    HandleTask(component_, task);
  });
}

/// @brief Add an event to the main loop.
/// It will be executed later, after every other scheduled events.
void App::PostEvent(Event event) {
  Post(event);
}

/// @brief Add a task to draw the screen one more time, until all the animations
/// are done.
void App::RequestAnimationFrame() {
  if (animation_requested_) {
    return;
  }
  animation_requested_ = true;
  auto now = animation::Clock::now();
  const auto time_histeresis = std::chrono::milliseconds(33);
  if (now - previous_animation_time_ >= time_histeresis) {
    previous_animation_time_ = now;
  }
}

/// @brief Try to get the unique lock about behing able to capture the mouse.
/// @return A unique lock if the mouse is not already captured, otherwise a
/// null.
CapturedMouse App::CaptureMouse() {
  if (mouse_captured) {
    return nullptr;
  }
  mouse_captured = true;
  return std::make_unique<CapturedMouseImpl>(
      [this] { mouse_captured = false; });
}

/// @brief Execute the main loop.
/// @param component The component to draw.
void App::Loop(Component component) {  // NOLINT
  class Loop loop(this, std::move(component));
  loop.Run();
}

/// @brief Return whether the main loop has been quit.
bool App::HasQuitted() {
  return quit_;
}

// private
void App::PreMain() {
  // Suspend previously active screen:
  if (g_active_screen) {
    std::swap(suspended_screen_, g_active_screen);
    // Reset cursor position to the top of the screen and clear the screen.
    suspended_screen_->TerminalSend(suspended_screen_->ResetCursorPosition());
    suspended_screen_->ResetPosition(suspended_screen_->internal_->output_buffer,
                                     /*clear=*/true);
    suspended_screen_->dimx_ = 0;
    suspended_screen_->dimy_ = 0;

    // Reset dimensions to force drawing the screen again next time:
    suspended_screen_->Uninstall();
  }

  // This screen is now active:
  g_active_screen = this;
  g_active_screen->Install();

  previous_animation_time_ = animation::Clock::now();
}

// private
void App::PostMain() {
  // Put cursor position at the end of the drawing.
  TerminalSend(ResetCursorPosition());

  g_active_screen = nullptr;

  // Restore suspended screen.
  if (suspended_screen_) {
    // Clear screen, and put the cursor at the beginning of the drawing.
    ResetPosition(internal_->output_buffer, /*clear=*/true);
    dimx_ = 0;
    dimy_ = 0;
    Uninstall();
    std::swap(g_active_screen, suspended_screen_);
    g_active_screen->Install();
  } else {
    Uninstall();

    std::cout << "\r";
    // On final exit, keep the current drawing and reset cursor position one
    // line after it.
    if (!use_alternative_screen_) {
      std::cout << "\n";
    }
    std::cout << std::flush;
  }
}

/// @brief Decorate a function. It executes the same way, but with the currently
/// active screen terminal hooks temporarilly uninstalled during its execution.
/// @param fn The function to decorate.
Closure App::WithRestoredIO(Closure fn) {  // NOLINT
  return [this, fn] {
    Uninstall();
    fn();
    Install();
  };
}

/// @brief Force FTXUI to handle or not handle Ctrl-C, even if the component
/// catches the Event::CtrlC.
void App::ForceHandleCtrlC(bool force) {
  force_handle_ctrl_c_ = force;
}

/// @brief Force FTXUI to handle or not handle Ctrl-Z, even if the component
/// catches the Event::CtrlZ.
void App::ForceHandleCtrlZ(bool force) {
  force_handle_ctrl_z_ = force;
}

/// @brief Returns the content of the current selection
std::string App::GetSelection() {
  if (!selection_) {
    return "";
  }
  return selection_->GetParts();
}

void App::SelectionChange(std::function<void()> callback) {
  selection_on_change_ = std::move(callback);
}

// ACECODE-PATCH(drag-autoscroll): see app.hpp for rationale. Adds dx/dy to
// start/end coordinates so a caller that just scrolled content by N rows can
// keep the previously-anchored text under the same effective selection. Resets
// selection_data_previous_ to force RefreshSelection() to re-run on the next
// frame even if the diff would have looked the same.
void App::ShiftSelection(int dx, int dy) {
  if (selection_data_.empty) {
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("ShiftSelection skipped empty dx=" + std::to_string(dx) +
                 " dy=" + std::to_string(dy) + " frame=" +
                 std::to_string(frame_count_));
#endif
    return;
  }
#if ACECODE_TUI_INPUT_TRACE
  AcecodeTrace("ShiftSelection apply dx=" + std::to_string(dx) +
               " dy=" + std::to_string(dy) + " before=(" +
               std::to_string(selection_data_.start_x) + "," +
               std::to_string(selection_data_.start_y) + ")->(" +
               std::to_string(selection_data_.end_x) + "," +
               std::to_string(selection_data_.end_y) + ") frame=" +
               std::to_string(frame_count_));
#endif
  selection_data_.start_x += dx;
  selection_data_.start_y += dy;
  selection_data_.end_x += dx;
  selection_data_.end_y += dy;
#if ACECODE_TUI_INPUT_TRACE
  AcecodeTrace("ShiftSelection after=(" +
               std::to_string(selection_data_.start_x) + "," +
               std::to_string(selection_data_.start_y) + ")->(" +
               std::to_string(selection_data_.end_x) + "," +
               std::to_string(selection_data_.end_y) + ") frame=" +
               std::to_string(frame_count_));
#endif
  // Force the next RunOnce to detect a diff and re-resolve the selection tree.
  selection_data_previous_.start_x = -999999;
  frame_valid_ = false;
}

bool App::HasPendingSelection() const {
  return static_cast<bool>(selection_pending_);
}

/// @brief Return the currently active screen, or null if none.
// static
App* App::Active() {
  return g_active_screen;
}

// private
void App::Install() {
  frame_valid_ = false;
  mouse_tracking_enabled_ = false;
  defer_mouse_tracking_until_cursor_position_ = false;
#if ACECODE_TUI_INPUT_TRACE
  AcecodeTrace("Install begin track_mouse=" +
               std::to_string(track_mouse_ ? 1 : 0) +
               " alt_screen=" +
               std::to_string(use_alternative_screen_ ? 1 : 0) +
               " terminal_output=" +
               std::to_string(
                   dimension_ == Dimension::TerminalOutput ? 1 : 0) +
               " frame=" + std::to_string(frame_count_) + " cursor=(" +
               std::to_string(cursor_x_) + "," +
               std::to_string(cursor_y_) + ")");
#endif

  // Flush the buffer for stdout to ensure whatever the user has printed before
  // is fully applied before we start modifying the terminal configuration. This
  // is important, because we are using two different channels (stdout vs
  // termios/WinAPI) to communicate with the terminal emulator below. See
  // https://github.com/ArthurSonzogni/FTXUI/issues/846
  TerminalFlush();

  InstallPipedInputHandling();

  // After uninstalling the new configuration, flush it to the terminal to
  // ensure it is fully applied:
  on_exit_functions.emplace([this] {
#if defined(_WIN32)
    // Windows console modes are restored by other on-exit callbacks before
    // this final flush runs. In legacy cmd.exe that disables VT processing,
    // so any buffered cleanup sequences would be printed as raw ESC text.
    // Temporarily re-enable VT for the flush, then put the mode back.
    auto stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD out_mode = 0;
    const bool restore_mode =
        stdout_handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(stdout_handle, &out_mode);
    if (restore_mode) {
      constexpr DWORD enable_virtual_terminal_processing = 0x0004;
      constexpr DWORD disable_newline_auto_return = 0x0008;
      SetConsoleMode(stdout_handle,
                     out_mode | enable_virtual_terminal_processing |
                         disable_newline_auto_return);
    }
    TerminalFlush();
    if (restore_mode) {
      SetConsoleMode(stdout_handle, out_mode);
    }
#else
    TerminalFlush();
#endif
  });

  // Request the terminal to report the current cursor shape. We will restore it
  // on exit.
  TerminalSend(DECRQSS_DECSCUSR);
  on_exit_functions.emplace([this] {
    TerminalSend("\033[?25h");  // Enable cursor.
    TerminalSend("\033[" + std::to_string(cursor_reset_shape_) + " q");
  });

  // Install signal handlers to restore the terminal state on exit. The default
  // signal handlers are restored on exit.
  for (const int signal : {SIGTERM, SIGSEGV, SIGINT, SIGILL, SIGABRT, SIGFPE}) {
    InstallSignalHandler(signal);
  }

// Save the old terminal configuration and restore it on exit.
#if defined(_WIN32)
  // Enable VT processing on stdout and stdin
  auto stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);

  DWORD out_mode = 0;
  DWORD in_mode = 0;
  GetConsoleMode(stdout_handle, &out_mode);
  GetConsoleMode(stdin_handle, &in_mode);
  on_exit_functions.push([=] { SetConsoleMode(stdout_handle, out_mode); });
  on_exit_functions.push([=] { SetConsoleMode(stdin_handle, in_mode); });

  // https://docs.microsoft.com/en-us/windows/console/setconsolemode
  const int enable_virtual_terminal_processing = 0x0004;
  const int disable_newline_auto_return = 0x0008;
  out_mode |= enable_virtual_terminal_processing;
  out_mode |= disable_newline_auto_return;

  // https://docs.microsoft.com/en-us/windows/console/setconsolemode
  const int enable_line_input = 0x0002;
  const int enable_echo_input = 0x0004;
  const int enable_virtual_terminal_input = 0x0200;
  const int enable_window_input = 0x0008;
  in_mode &= ~enable_echo_input;
  in_mode &= ~enable_line_input;
  in_mode |= enable_virtual_terminal_input;
  in_mode |= enable_window_input;

  SetConsoleMode(stdin_handle, in_mode);
  SetConsoleMode(stdout_handle, out_mode);
#else  // POSIX (Linux & Mac)
  // #if defined(__EMSCRIPTEN__)
  //// Reading stdin isn't blocking.
  // int flags = fcntl(0, F_GETFL, 0);
  // fcntl(0, F_SETFL, flags | O_NONBLOCK);

  //// Restore the terminal configuration on exit.
  // on_exit_functions.emplace([flags] { fcntl(0, F_SETFL, flags); });
  // #endif
  for (const int signal : {SIGWINCH, SIGTSTP}) {
    InstallSignalHandler(signal);
  }

  struct termios terminal;  // NOLINT
  tcgetattr(tty_fd_, &terminal);
  on_exit_functions.emplace([terminal = terminal, tty_fd_ = tty_fd_] {
    tcsetattr(tty_fd_, TCSANOW, &terminal);
  });

  // Enabling raw terminal input mode
  terminal.c_iflag &= ~IGNBRK;  // Disable ignoring break condition
  terminal.c_iflag &= ~BRKINT;  // Disable break causing input and output to be
                                // flushed
  terminal.c_iflag &= ~PARMRK;  // Disable marking parity errors.
  terminal.c_iflag &= ~ISTRIP;  // Disable striping 8th bit off characters.
  terminal.c_iflag &= ~INLCR;   // Disable mapping NL to CR.
  terminal.c_iflag &= ~IGNCR;   // Disable ignoring CR.
  terminal.c_iflag &= ~ICRNL;   // Disable mapping CR to NL.
  terminal.c_iflag &= ~IXON;    // Disable XON/XOFF flow control on output

  terminal.c_lflag &= ~ECHO;    // Disable echoing input characters.
  terminal.c_lflag &= ~ECHONL;  // Disable echoing new line characters.
  terminal.c_lflag &= ~ICANON;  // Disable Canonical mode.
  terminal.c_lflag &= ~ISIG;    // Disable sending signal when hitting:
                                // -     => DSUSP
                                // - C-Z => SUSP
                                // - C-C => INTR
                                // - C-d => QUIT
  terminal.c_lflag &= ~IEXTEN;  // Disable extended input processing
  terminal.c_cflag |= CS8;      // 8 bits per byte

  terminal.c_cc[VMIN] = 0;   // Minimum number of characters for non-canonical
                             // read.
  terminal.c_cc[VTIME] = 0;  // Timeout in deciseconds for non-canonical read.

  tcsetattr(tty_fd_, TCSANOW, &terminal);

#endif

  auto enable = [&](const std::vector<DECMode>& parameters) {
    TerminalSend(Set(parameters));
    on_exit_functions.emplace(
        [this, parameters] { TerminalSend(Reset(parameters)); });
  };

  auto disable = [&](const std::vector<DECMode>& parameters) {
    TerminalSend(Reset(parameters));
    on_exit_functions.emplace(
        [this, parameters] { TerminalSend(Set(parameters)); });
  };

  if (use_alternative_screen_) {
    enable({
        DECMode::kAlternateScreen,
    });
  }

  disable({
      // DECMode::kCursor,
      DECMode::kLineWrap,
  });

  if (track_mouse_) {
    on_exit_functions.emplace(
        [this] { TerminalSend(Reset({DECMode::kMouseVt200})); });
    on_exit_functions.emplace(
        [this] { TerminalSend(Reset({DECMode::kMouseBtnEventMouse})); });
    on_exit_functions.emplace(
        [this] { TerminalSend(Reset({DECMode::kMouseUrxvtMode})); });
    on_exit_functions.emplace(
        [this] { TerminalSend(Reset({DECMode::kMouseSgrExtMode})); });

    // ACECODE-PATCH(mouse-origin): In TerminalOutput mode, FTXUI must learn
    // the frame origin from a cursor-position report before translating mouse
    // coordinates. If mouse tracking is enabled before that report is handled,
    // early drag-selection events can be translated with a stale origin and
    // render the highlighted selection several rows above the pointer.
    defer_mouse_tracking_until_cursor_position_ =
        IsTerminalOutputPrimaryScreen();
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("Install mouse setup defer_until_cursor_position=" +
                 std::to_string(
                     defer_mouse_tracking_until_cursor_position_ ? 1 : 0) +
                 " frame=" + std::to_string(frame_count_) + " cursor=(" +
                 std::to_string(cursor_x_) + "," +
                 std::to_string(cursor_y_) + ")");
#endif
    if (!defer_mouse_tracking_until_cursor_position_) {
      EnableMouseTracking(false);
    }
  }

  // After installing the new configuration, flush it to the terminal to
  // ensure it is fully applied:
  TerminalFlush();

  quit_ = false;

  PostAnimationTask();
#if ACECODE_TUI_INPUT_TRACE
  AcecodeTrace("Install end mouse_tracking_enabled=" +
               std::to_string(mouse_tracking_enabled_ ? 1 : 0) +
               " defer_until_cursor_position=" +
               std::to_string(
                   defer_mouse_tracking_until_cursor_position_ ? 1 : 0) +
               " frame=" + std::to_string(frame_count_));
#endif
}

void App::InstallPipedInputHandling() {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  tty_fd_ = STDIN_FILENO;
  // Handle piped input redirection if explicitly enabled by the application.
  // This allows applications to read data from stdin while still receiving
  // keyboard input from the terminal for interactive use.
  if (!handle_piped_input_) {
    return;
  }

  // If stdin is a terminal, we don't need to open /dev/tty.
  if (isatty(STDIN_FILENO)) {
    return;
  }

  // Open /dev/tty for keyboard input.
  tty_fd_ = open("/dev/tty", O_RDONLY);
  if (tty_fd_ < 0) {
    // Failed to open /dev/tty (containers, headless systems, etc.)
    tty_fd_ = STDIN_FILENO;  // Fallback to stdin.
    return;
  }

  // Close the /dev/tty file descriptor on exit.
  on_exit_functions.emplace([this] {
    close(tty_fd_);
    tty_fd_ = -1;
  });
#endif
}

// private
void App::Uninstall() {
  ExitNow();
  OnExit();
}

// private
// NOLINTNEXTLINE
void App::RunOnceBlocking(Component component) {
  // Set FPS to 60 at most.
  const auto time_per_frame = std::chrono::microseconds(16666);  // 1s / 60fps

  auto time = std::chrono::steady_clock::now();
  size_t executed_task = internal_->task_runner.ExecutedTasks();

  // Wait for at least one task to execute.
  while (executed_task == internal_->task_runner.ExecutedTasks() &&
         !HasQuitted()) {
    RunOnce(component);

    const auto now = std::chrono::steady_clock::now();
    const auto delta = now - time;
    time = now;

    if (delta < time_per_frame) {
      const auto sleep_duration = time_per_frame - delta;
      std::this_thread::sleep_for(sleep_duration);
    }
  }
}

// private
void App::RunOnce(Component component) {
  AutoReset set_component(&component_, component);
  ExecuteSignalHandlers();
  FetchTerminalEvents();

  // Execute the pending tasks from the queue.
  const size_t executed_task = internal_->task_runner.ExecutedTasks();
  internal_->task_runner.RunUntilIdle();
  // If no executed task, we can return early without redrawing the screen.
  if (executed_task == internal_->task_runner.ExecutedTasks()) {
    return;
  }

  ExecuteSignalHandlers();
  Draw(component);

  if (selection_data_previous_ != selection_data_) {
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("SelectionData changed previous=(" +
                 std::to_string(selection_data_previous_.start_x) + "," +
                 std::to_string(selection_data_previous_.start_y) + ")->(" +
                 std::to_string(selection_data_previous_.end_x) + "," +
                 std::to_string(selection_data_previous_.end_y) + ") empty=" +
                 std::to_string(selection_data_previous_.empty ? 1 : 0) +
                 " current=(" + std::to_string(selection_data_.start_x) +
                 "," + std::to_string(selection_data_.start_y) + ")->(" +
                 std::to_string(selection_data_.end_x) + "," +
                 std::to_string(selection_data_.end_y) + ") empty=" +
                 std::to_string(selection_data_.empty ? 1 : 0) +
                 " frame=" + std::to_string(frame_count_));
#endif
    selection_data_previous_ = selection_data_;
    if (selection_on_change_) {
      selection_on_change_();
      Post(Event::Custom);
    }
  }
}

// private
// NOLINTNEXTLINE
void App::HandleTask(Component component, Task& task) {
  std::visit(
      [&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;

        // clang-format off
    // Handle Event.
    if constexpr (std::is_same_v<T, Event>) {

      if (arg.is_cursor_position()) {
        const int previous_cursor_x = cursor_x_;
        const int previous_cursor_y = cursor_y_;
        const int reported_cursor_x = arg.cursor_x();
        const int reported_cursor_y = arg.cursor_y();
        const bool usable_cursor_position =
            CursorPositionIsUsable(reported_cursor_x, reported_cursor_y);
#if ACECODE_TUI_INPUT_TRACE
        AcecodeTrace("CursorPosition raw=(" +
                     std::to_string(reported_cursor_x) + "," +
                     std::to_string(reported_cursor_y) + ") previous=(" +
                     std::to_string(previous_cursor_x) + "," +
                     std::to_string(previous_cursor_y) + ") usable=" +
                     std::to_string(usable_cursor_position ? 1 : 0) +
                     " dim=(" + std::to_string(dimx_) + "," +
                     std::to_string(dimy_) + ") frame=" +
                     std::to_string(frame_count_) +
                     " mouse_tracking_enabled=" +
                     std::to_string(mouse_tracking_enabled_ ? 1 : 0) +
                     " defer_until_cursor_position=" +
                     std::to_string(
                         defer_mouse_tracking_until_cursor_position_ ? 1 : 0));
#endif
        if (!usable_cursor_position) {
          if (IsTerminalOutputPrimaryScreen()) {
            defer_mouse_tracking_until_cursor_position_ = true;
          }
          frame_valid_ = false;
          PostAnimationTask();
#if ACECODE_TUI_INPUT_TRACE
          AcecodeTrace("CursorPosition rejected unusable raw=(" +
                       std::to_string(reported_cursor_x) + "," +
                       std::to_string(reported_cursor_y) +
                       ") keeping cursor=(" + std::to_string(cursor_x_) +
                       "," + std::to_string(cursor_y_) + ") frame=" +
                       std::to_string(frame_count_));
#endif
          return;
        }

        cursor_x_ = reported_cursor_x;
        cursor_y_ = reported_cursor_y;
        if (defer_mouse_tracking_until_cursor_position_ && frame_count_ > 0) {
#if ACECODE_TUI_INPUT_TRACE
          AcecodeTrace("CursorPosition enabling deferred mouse tracking frame=" +
                       std::to_string(frame_count_) + " cursor=(" +
                       std::to_string(cursor_x_) + "," +
                       std::to_string(cursor_y_) + ")");
#endif
          defer_mouse_tracking_until_cursor_position_ = false;
          EnableMouseTracking(true);
        }
        return;
      }

      if (arg.is_cursor_shape()) {
        cursor_reset_shape_= arg.cursor_shape();
        return;
      }

#if ACECODE_TUI_INPUT_TRACE
      bool trace_mouse = false;
      Mouse raw_mouse;
#endif
      if (arg.is_mouse()) {
#if ACECODE_TUI_INPUT_TRACE
        raw_mouse = arg.mouse();
        trace_mouse = TraceMouseEvent(raw_mouse);
        if (trace_mouse) {
          AcecodeTrace("Mouse raw " + MouseForTrace(raw_mouse) +
                       " cursor=(" + std::to_string(cursor_x_) + "," +
                       std::to_string(cursor_y_) + ") frame=" +
                       std::to_string(frame_count_) +
                       " mouse_tracking_enabled=" +
                       std::to_string(mouse_tracking_enabled_ ? 1 : 0));
        }
#endif
        if (!CursorPositionIsUsable(cursor_x_, cursor_y_)) {
#if ACECODE_TUI_INPUT_TRACE
          if (trace_mouse) {
            AcecodeTrace("Mouse detected unusable cursor origin cursor=(" +
                         std::to_string(cursor_x_) + "," +
                         std::to_string(cursor_y_) + ") dim=(" +
                         std::to_string(dimx_) + "," +
                         std::to_string(dimy_) +
                         "), falling back to (1,1) and requesting DSR");
          }
#endif
          cursor_x_ = 1;
          cursor_y_ = 1;
          if (IsTerminalOutputPrimaryScreen()) {
            defer_mouse_tracking_until_cursor_position_ = true;
          }
          frame_valid_ = false;
          PostAnimationTask();
        }
        arg.mouse().x -= cursor_x_;
        arg.mouse().y -= cursor_y_;
#if ACECODE_TUI_INPUT_TRACE
        if (trace_mouse) {
          AcecodeTrace("Mouse adjusted " + MouseForTrace(arg.mouse()) +
                       " from_raw=(" + std::to_string(raw_mouse.x) + "," +
                       std::to_string(raw_mouse.y) + ") cursor=(" +
                       std::to_string(cursor_x_) + "," +
                       std::to_string(cursor_y_) + ") frame=" +
                       std::to_string(frame_count_));
        }
#endif
      }

      arg.screen_ = this;

      bool handled = component->OnEvent(arg);
#if ACECODE_TUI_INPUT_TRACE
      if (trace_mouse) {
        AcecodeTrace("Mouse after component handled=" +
                     std::to_string(handled ? 1 : 0) + " " +
                     MouseForTrace(arg.mouse()) + " frame=" +
                     std::to_string(frame_count_));
      }
#endif

      handled = HandleSelection(handled, arg);
#if ACECODE_TUI_INPUT_TRACE
      if (trace_mouse) {
        AcecodeTrace("Mouse after selection handled=" +
                     std::to_string(handled ? 1 : 0) + " empty=" +
                     std::to_string(selection_data_.empty ? 1 : 0) +
                     " data=(" + std::to_string(selection_data_.start_x) +
                     "," + std::to_string(selection_data_.start_y) +
                     ")->(" + std::to_string(selection_data_.end_x) + "," +
                     std::to_string(selection_data_.end_y) + ") pending=" +
                     std::to_string(selection_pending_ ? 1 : 0) +
                     " frame=" + std::to_string(frame_count_));
      }
#endif

      if (arg == Event::CtrlC && (!handled || force_handle_ctrl_c_)) {
        RecordSignal(SIGABRT);
      }

#if !defined(_WIN32)
      if (arg == Event::CtrlZ && (!handled || force_handle_ctrl_z_)) {
        RecordSignal(SIGTSTP);
      }
#endif
      
      frame_valid_ = false;
      return;
    }

    // Handle callback
    if constexpr (std::is_same_v<T, Closure>) {
      arg();
      return;
    }

    // Handle Animation
    if constexpr (std::is_same_v<T, AnimationTask>) {
      if (!animation_requested_) {
        return;
      }

      animation_requested_ = false;
      const animation::TimePoint now = animation::Clock::now();
      const animation::Duration delta = now - previous_animation_time_;
      previous_animation_time_ = now;

      animation::Params params(delta);
      component->OnAnimation(params);
      frame_valid_ = false;
      return;
    }
  },
  task);
  // clang-format on
}

// private
bool App::HandleSelection(bool handled, Event event) {
  if (handled) {
#if ACECODE_TUI_INPUT_TRACE
    if (event.is_mouse() && TraceMouseEvent(event.mouse())) {
      AcecodeTrace("HandleSelection clear handled=1 " +
                   MouseForTrace(event.mouse()) + " previous_empty=" +
                   std::to_string(selection_data_.empty ? 1 : 0) +
                   " previous_data=(" +
                   std::to_string(selection_data_.start_x) + "," +
                   std::to_string(selection_data_.start_y) + ")->(" +
                   std::to_string(selection_data_.end_x) + "," +
                   std::to_string(selection_data_.end_y) + ") pending=" +
                   std::to_string(selection_pending_ ? 1 : 0) +
                   " frame=" + std::to_string(frame_count_));
    }
#endif
    selection_pending_ = nullptr;
    selection_data_.empty = true;
    selection_ = nullptr;
    return true;
  }

  if (!event.is_mouse()) {
    return false;
  }

  auto& mouse = event.mouse();
  if (mouse.button != Mouse::Left) {
    return false;
  }

  if (mouse.motion == Mouse::Pressed) {
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("HandleSelection pressed capture " + MouseForTrace(mouse) +
                 " previous_empty=" +
                 std::to_string(selection_data_.empty ? 1 : 0) +
                 " previous_data=(" +
                 std::to_string(selection_data_.start_x) + "," +
                 std::to_string(selection_data_.start_y) + ")->(" +
                 std::to_string(selection_data_.end_x) + "," +
                 std::to_string(selection_data_.end_y) + ") frame=" +
                 std::to_string(frame_count_));
#endif
    selection_pending_ = CaptureMouse();
    selection_data_.start_x = mouse.x;
    selection_data_.start_y = mouse.y;
    selection_data_.end_x = mouse.x;
    selection_data_.end_y = mouse.y;
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("HandleSelection pressed after data=(" +
                 std::to_string(selection_data_.start_x) + "," +
                 std::to_string(selection_data_.start_y) + ")->(" +
                 std::to_string(selection_data_.end_x) + "," +
                 std::to_string(selection_data_.end_y) + ") pending=" +
                 std::to_string(selection_pending_ ? 1 : 0) +
                 " frame=" + std::to_string(frame_count_));
#endif
    return false;
  }

  if (!selection_pending_) {
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("HandleSelection ignored no_pending " + MouseForTrace(mouse) +
                 " empty=" + std::to_string(selection_data_.empty ? 1 : 0) +
                 " frame=" + std::to_string(frame_count_));
#endif
    return false;
  }

  if (mouse.motion == Mouse::Moved) {
#if ACECODE_TUI_INPUT_TRACE
    const int before_end_x = selection_data_.end_x;
    const int before_end_y = selection_data_.end_y;
    const bool before_empty = selection_data_.empty;
#endif
    if ((mouse.x != selection_data_.end_x) ||
        (mouse.y != selection_data_.end_y)) {
      selection_data_.end_x = mouse.x;
      selection_data_.end_y = mouse.y;
      selection_data_.empty = false;
    }

#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("HandleSelection moved " + MouseForTrace(mouse) +
                 " end=(" + std::to_string(before_end_x) + "," +
                 std::to_string(before_end_y) + ")->(" +
                 std::to_string(selection_data_.end_x) + "," +
                 std::to_string(selection_data_.end_y) + ") empty=" +
                 std::to_string(before_empty ? 1 : 0) + "->" +
                 std::to_string(selection_data_.empty ? 1 : 0) +
                 " start=(" + std::to_string(selection_data_.start_x) +
                 "," + std::to_string(selection_data_.start_y) +
                 ") frame=" + std::to_string(frame_count_));
#endif
    return true;
  }

  if (mouse.motion == Mouse::Released) {
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("HandleSelection released " + MouseForTrace(mouse) +
                 " before_data=(" +
                 std::to_string(selection_data_.start_x) + "," +
                 std::to_string(selection_data_.start_y) + ")->(" +
                 std::to_string(selection_data_.end_x) + "," +
                 std::to_string(selection_data_.end_y) + ") frame=" +
                 std::to_string(frame_count_));
#endif
    selection_pending_ = nullptr;
    selection_data_.end_x = mouse.x;
    selection_data_.end_y = mouse.y;
    selection_data_.empty = false;
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("HandleSelection released after data=(" +
                 std::to_string(selection_data_.start_x) + "," +
                 std::to_string(selection_data_.start_y) + ")->(" +
                 std::to_string(selection_data_.end_x) + "," +
                 std::to_string(selection_data_.end_y) + ") pending=" +
                 std::to_string(selection_pending_ ? 1 : 0) +
                 " frame=" + std::to_string(frame_count_));
#endif
    return true;
  }

  return false;
}

// private
// NOLINTNEXTLINE
void App::Draw(Component component) {
  if (frame_valid_) {
    return;
  }
  auto document = component->Render();
  int dimx = 0;
  int dimy = 0;
  auto terminal = Terminal::Size();
  document->ComputeRequirement();
  switch (dimension_) {
    case Dimension::Fixed:
      dimx = dimx_;
      dimy = dimy_;
      break;
    case Dimension::TerminalOutput:
      dimx = terminal.dimx;
      dimy = util::clamp(document->requirement().min_y, 0, terminal.dimy);
      break;
    case Dimension::Fullscreen:
      dimx = terminal.dimx;
      dimy = terminal.dimy;
      break;
    case Dimension::FitComponent:
      dimx = util::clamp(document->requirement().min_x, 0, terminal.dimx);
      dimy = util::clamp(document->requirement().min_y, 0, terminal.dimy);
      break;
  }

  // Hide cursor to prevent flickering during reset.
  TerminalSend("\033[?25l");

  const bool resized = frame_count_ == 0 || (dimx != dimx_) || (dimy != dimy_);
  TerminalSend(ResetCursorPosition());

  if (frame_count_ != 0) {
    // Reset the cursor position to the lower left corner to start drawing the
    // new frame. 
    ResetPosition(internal_->output_buffer, resized);

    // If the terminal width decrease, the terminal emulator will start wrapping
    // lines and make the display dirty. We should clear it completely.
    if ((dimx < dimx_) && !use_alternative_screen_) {
      TerminalSend("\033[J");  // clear terminal output
      TerminalSend("\033[H");  // move cursor to home position
    }
  }

  // Resize the screen if needed.
  if (resized) {
    dimx_ = dimx;
    dimy_ = dimy;
    cells_ = std::vector<std::vector<Cell>>(dimy, std::vector<Cell>(dimx));
    cursor_.x = dimx_ - 1;
    cursor_.y = dimy_ - 1;
  }

  // Periodically request the terminal emulator the frame position relative to
  // the screen. This is useful for converting mouse position reported in
  // screen's coordinates to frame's coordinates.
  const bool cursor_position_needs_refresh =
      IsTerminalOutputPrimaryScreen() &&
      (defer_mouse_tracking_until_cursor_position_ ||
       !CursorPositionIsUsable(cursor_x_, cursor_y_));
#if defined(FTXUI_MICROSOFT_TERMINAL_FALLBACK)
  // Microsoft's terminal suffers from a [bug]. When reporting the cursor
  // position, several output sequences are mixed together into garbage.
  // This causes FTXUI user to see some "1;1;R" sequences into the Input
  // component. See [issue]. Solution is to request cursor position less
  // often. [bug]: https://github.com/microsoft/terminal/pull/7583 [issue]:
  // https://github.com/ArthurSonzogni/FTXUI/issues/136
  static int i = -3;
  ++i;
  if (!use_alternative_screen_ &&
      (frame_count_ == 0 || previous_frame_resized_ ||
       cursor_position_needs_refresh || i % 150 == 0)) {  // NOLINT
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("Draw request cursor DSR frame=" +
                 std::to_string(frame_count_) + " resized=" +
                 std::to_string(resized ? 1 : 0) + " dim=(" +
                 std::to_string(dimx_) + "," + std::to_string(dimy_) +
                 ") cursor=(" + std::to_string(cursor_x_) + "," +
                 std::to_string(cursor_y_) + ") needs_refresh=" +
                 std::to_string(cursor_position_needs_refresh ? 1 : 0) +
                 " microsoft_fallback=1");
#endif
    TerminalSend(DeviceStatusReport(DSRMode::kCursor));
  }
#else
  static int i = -3;
  ++i;
  if (!use_alternative_screen_ &&
      (frame_count_ == 0 || previous_frame_resized_ ||
       cursor_position_needs_refresh || i % 40 == 0)) {  // NOLINT
#if ACECODE_TUI_INPUT_TRACE
    AcecodeTrace("Draw request cursor DSR frame=" +
                 std::to_string(frame_count_) + " resized=" +
                 std::to_string(resized ? 1 : 0) +
                 " previous_frame_resized=" +
                 std::to_string(previous_frame_resized_ ? 1 : 0) +
                 " dim=(" + std::to_string(dimx_) + "," +
                 std::to_string(dimy_) + ") cursor=(" +
                 std::to_string(cursor_x_) + "," +
                 std::to_string(cursor_y_) + ") needs_refresh=" +
                 std::to_string(cursor_position_needs_refresh ? 1 : 0) +
                 " microsoft_fallback=0");
#endif
    TerminalSend(DeviceStatusReport(DSRMode::kCursor));
  }
#endif
  previous_frame_resized_ = resized;

  selection_ = selection_data_.empty
                   ? std::make_unique<Selection>()
                   : std::make_unique<Selection>(
                         selection_data_.start_x, selection_data_.start_y,  //
                         selection_data_.end_x, selection_data_.end_y);
  Render(*this, document.get(), *selection_);

  // Set cursor position for user using tools to insert CJK characters.
  {
    const int dx = dimx_ - 1 - cursor_.x + int(dimx_ != terminal.dimx);
    const int dy = dimy_ - 1 - cursor_.y;

    set_cursor_position_.clear();
    reset_cursor_position_.clear();

    if (dy != 0) {
      set_cursor_position_ += "\x1B[" + std::to_string(dy) + "A";
      reset_cursor_position_ += "\x1B[" + std::to_string(dy) + "B";
    }

    if (dx != 0) {
      set_cursor_position_ += "\x1B[" + std::to_string(dx) + "D";
      reset_cursor_position_ += "\x1B[" + std::to_string(dx) + "C";
    }

    if (cursor_.shape != Cursor::Hidden) {
      set_cursor_position_ += "\033[?25h";
      set_cursor_position_ +=
          "\033[" + std::to_string(int(cursor_.shape)) + " q";
    }
  }

  ToString(internal_->output_buffer);
  TerminalSend(set_cursor_position_);
  TerminalFlush();

  Clear();
  frame_valid_ = true;
  frame_count_++;
}

// private
std::string App::ResetCursorPosition() {
  std::string result = std::move(reset_cursor_position_);
  reset_cursor_position_= "";
  return result;
}

// private
void App::TerminalSend(std::string_view s) {
  internal_->output_buffer += s;
}

// private
void App::TerminalFlush() {
  // Emscripten doesn't implement flush. We interpret zero as flush.
  internal_->output_buffer += '\0';
  std::cout << internal_->output_buffer << std::flush;
  internal_->output_buffer.clear();
}

/// @brief Return a function to exit the main loop.
Closure App::ExitLoopClosure() {
  return [this] { Exit(); };
}

/// @brief Exit the main loop.
void App::Exit() {
  Post([this] { ExitNow(); });
}

// private:
void App::ExitNow() {
  quit_ = true;
}

// private:
void App::Signal(int signal) {
  if (signal == SIGABRT) {
    Exit();
    return;
  }

// Windows do no support SIGTSTP / SIGWINCH
#if !defined(_WIN32)
  if (signal == SIGTSTP) {
    Post([&] {
      TerminalSend(ResetCursorPosition());
      ResetPosition(internal_->output_buffer, /*clear*/ true);
      Uninstall();
      dimx_ = 0;
      dimy_ = 0;
      std::raise(SIGTSTP);
      Install();
    });
    return;
  }

  if (signal == SIGWINCH) {
    Post(Event::Special({0}));
    return;
  }
#endif
}

void App::FetchTerminalEvents() {
#if defined(_WIN32)
  auto get_input_records = [&]() -> std::vector<INPUT_RECORD> {
    // Check if there is input in the console.
    auto console = GetStdHandle(STD_INPUT_HANDLE);
    DWORD number_of_events = 0;
    if (!GetNumberOfConsoleInputEvents(console, &number_of_events)) {
      return std::vector<INPUT_RECORD>();
    }
    if (number_of_events <= 0) {
      // No input, return.
      return std::vector<INPUT_RECORD>();
    }
    // Read the input events.
    std::vector<INPUT_RECORD> records(number_of_events);
    DWORD number_of_events_read = 0;
    if (!ReadConsoleInput(console, records.data(), (DWORD)records.size(),
                          &number_of_events_read)) {
      return std::vector<INPUT_RECORD>();
    }
    records.resize(number_of_events_read);
    return records;
  };

  auto records = get_input_records();
  if (records.size() == 0) {
    const auto timeout =
        std::chrono::steady_clock::now() - internal_->last_char_time;
    const size_t timeout_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
    internal_->terminal_input_parser.Timeout(timeout_microseconds);
    return;
  }
  internal_->last_char_time = std::chrono::steady_clock::now();

  // Convert the input events to FTXUI events.
  // For each event, we call the terminal input parser to convert it to
  // Event.
  std::wstring wstring;
  for (const auto& r : records) {
    switch (r.EventType) {
      case KEY_EVENT: {
        auto key_event = r.Event.KeyEvent;
        // ignore UP key events
        if (key_event.bKeyDown == FALSE) {
          continue;
        }
        const wchar_t wc = key_event.uChar.UnicodeChar;
        wstring += wc;
        if (wc >= 0xd800 && wc <= 0xdbff) {
          // Wait for the Low Surrogate to arrive in the next record.
          continue;
        }
        for (auto it : to_string(wstring)) {
          internal_->terminal_input_parser.Add(it);
        }
        wstring.clear();
      } break;
      case WINDOW_BUFFER_SIZE_EVENT:
        Post(Event::Special({0}));
        break;
      case MENU_EVENT:
      case FOCUS_EVENT:
      case MOUSE_EVENT:
        // TODO(mauve): Implement later.
        break;
    }
  }
#elif defined(__EMSCRIPTEN__)
  // Read chars from the terminal.
  // We configured it to be non blocking.
  std::array<char, 128> out{};
  size_t l = read(STDIN_FILENO, out.data(), out.size());
  if (l == 0) {
    const auto timeout =
        std::chrono::steady_clock::now() - internal_->last_char_time;
    const size_t timeout_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
    internal_->terminal_input_parser.Timeout(timeout_microseconds);
    return;
  }
  internal_->last_char_time = std::chrono::steady_clock::now();

  // Convert the chars to events.
  for (size_t i = 0; i < l; ++i) {
    internal_->terminal_input_parser.Add(out[i]);
  }
#else  // POSIX (Linux & Mac)
  if (!CheckStdinReady(tty_fd_)) {
    const auto timeout =
        std::chrono::steady_clock::now() - internal_->last_char_time;
    const size_t timeout_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
    internal_->terminal_input_parser.Timeout(timeout_ms);
    return;
  }
  internal_->last_char_time = std::chrono::steady_clock::now();

  // Read chars from the terminal.
  std::array<char, 128> out{};
  size_t l = read(tty_fd_, out.data(), out.size());

  // Convert the chars to events.
  for (size_t i = 0; i < l; ++i) {
    internal_->terminal_input_parser.Add(out[i]);
  }
#endif
}

void App::PostAnimationTask() {
  Post(AnimationTask());

  // Repeat the animation task every 15ms. This correspond to a frame rate
  // of around 66fps.
  internal_->task_runner.PostDelayedTask([this] { PostAnimationTask(); },
                                         std::chrono::milliseconds(15));
}

bool App::SelectionData::operator==(const App::SelectionData& other) const {
  if (empty && other.empty) {
    return true;
  }
  if (empty || other.empty) {
    return false;
  }
  return start_x == other.start_x && start_y == other.start_y &&
         end_x == other.end_x && end_y == other.end_y;
}

bool App::SelectionData::operator!=(const App::SelectionData& other) const {
  return !(*this == other);
}

}  // namespace ftxui
