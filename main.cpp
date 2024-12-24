#include <windows.h>
#include <vector>
#include <iostream>
#include <memory>
#include <functional>
#include <algorithm>
#include <queue>
#include <mutex>
#include <string>
#include <shellscalingapi.h>
#pragma comment(lib, "Shcore.lib")

// Define MOD key (can be changed to MOD_CONTROL, MOD_WIN, etc.)
const UINT MOD_KEY = MOD_ALT;

// Define the color of the outline of the current focused window
const unsigned int FOCUSED_OUTLINE_COLOR = 0xFF5733;

const int OUTLINE_THICKNESS = 1;


// Global variables for the overlay window
HWND g_hOverlay = NULL;
COLORREF g_BorderColor = RGB(0, 255, 0);
int g_BorderThickness = 5;

// Forward declarations
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);



// Enumeration for split orientation
enum class SplitType {
    VERTICAL,   // Split into columns (left/right)
    HORIZONTAL  // Split into rows (top/bottom)
};

// Enumeration for navigation directions
enum class Direction {
    UP,
    DOWN,
    LEFT,
    RIGHT
};

// Structure to hold window information for fullscreen toggling
struct WindowInfo {
    HWND hwnd;
    RECT savedRect;            // Saved original position and size for fullscreen toggle
    LONG savedStyle;           // Saved original style for fullscreen toggle
    bool isFullscreen = false; // Track fullscreen state
};

// Structure to represent each node in the layout tree
struct LayoutNode {
    bool isSplit; // Indicates whether this node is a split or a leaf (window)

    // Split details (valid only if isSplit is true)
    SplitType splitType;
    float splitRatio; // e.g., 0.5 for equal split

    // Child nodes (valid only if isSplit is true)
    std::unique_ptr<LayoutNode> firstChild;
    std::unique_ptr<LayoutNode> secondChild;

    // Parent node pointer (useful for traversal)
    LayoutNode* parent;

    // Window information (valid only if isSplit is false)
    WindowInfo windowInfo;

    // Rectangle representing window position and size
    RECT windowRect;

    // Constructors
    // For leaf nodes
    LayoutNode(HWND window)
        : isSplit(false), splitType(SplitType::VERTICAL), splitRatio(0.5f),
          firstChild(nullptr), secondChild(nullptr), parent(nullptr),
          windowInfo{ window, RECT{}, 0, false }, windowRect{ 0,0,0,0 } {}

    // For split nodes
    LayoutNode(SplitType type, float ratio,
        std::unique_ptr<LayoutNode> first,
        std::unique_ptr<LayoutNode> second)
        : isSplit(true), splitType(type), splitRatio(ratio),
          firstChild(std::move(first)), secondChild(std::move(second)),
          parent(nullptr), windowInfo{ nullptr, RECT{}, 0, false }, windowRect{ 0,0,0,0 } {}
};

// Structure to hold queue items with node and its depth
struct QueueItem {
    LayoutNode* node;
    int depth;
};

// Global root of the layout tree
std::unique_ptr<LayoutNode> root;

// Vector to store all managed windows
std::vector<WindowInfo> managedWindows;

// Mutex for thread safety
std::mutex layoutMutex;

// Hotkey and resize mode variables
bool isResizeMode = false;
LayoutNode* activeNodeForResize = nullptr;
HHOOK hKeyboardHook = NULL;

// Queue to manage pending splits awaiting window assignments
std::queue<LayoutNode*> pendingSplits;

// Function Prototypes
bool MoveWindowNormalized(HWND hwnd, int x, int y, int width, int height);
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam);
void InitializeLayout(HWND firstWindow);
void AddWindowBreadthFirst(HWND newWindow, float splitRatio = 0.5f);
void ApplyLayout(LayoutNode* node, RECT area);
void TileWindows(const RECT& screenRect);
void SetWindowFullscreen(LayoutNode* node, const RECT& monitorRect);
LayoutNode* FindLayoutNode(LayoutNode* node, HWND hwnd);
void CollectLeafNodes(LayoutNode* node, std::vector<LayoutNode*>& leaves);
bool SwapWindowHandles(LayoutNode* nodeA, LayoutNode* nodeB);
void FocusWindow(LayoutNode* node);
bool RegisterHotKeys();
void UnregisterHotKeys();
LayoutNode* FindAdjacent(LayoutNode* current, Direction dir);
void Navigate(Direction dir);
bool MoveWindowInDirection(Direction dir);
bool AddNewSplit(LayoutNode* currentNode, Direction dir);
SplitType GetSplitTypeFromDirection(Direction dir);
void AdjustSplitRatio(LayoutNode* node, float deltaRatio);
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
void PrintLayout(LayoutNode* node, int depth = 0);
void ChangeSplitOrientation(SplitType newSplitType);
void CALLBACK WinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD dwEventThread,
    DWORD dwmsEventTime
);

// Helper function to retrieve window title
std::string GetWindowTitle(HWND hwnd) {
    // First, try ANSI version
    char titleA[512];
    int lengthA = GetWindowTextA(hwnd, titleA, sizeof(titleA));
    if (lengthA > 0) {
        return std::string(titleA);
    }

    // If ANSI fails, try Unicode
    wchar_t titleW[512];
    int lengthW = GetWindowTextW(hwnd, titleW, sizeof(titleW)/sizeof(wchar_t));
    if (lengthW > 0) {
        // Convert wchar_t to std::string (UTF-8)
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, titleW, lengthW, NULL, 0, NULL, NULL);
        std::string title(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, titleW, lengthW, &title[0], size_needed, NULL, NULL);
        return title;
    }

    return ""; // No title found
}

// Function to normalize and move windows for more consistent tiling behavior
bool MoveWindowNormalized(HWND hwnd, int x, int y, int width, int height) {
    if (!hwnd) return false;

    // Validate the window handle
    if (!IsWindow(hwnd)) {
        std::cerr << "MoveWindowNormalized: Invalid HWND.\n";
        return false;
    }

    // Retrieve original style
    LONG originalStyle = GetWindowLong(hwnd, GWL_STYLE);
    if (originalStyle == 0 && GetLastError() != 0) {
        std::cerr << "MoveWindowNormalized: Failed to get window style for HWND=0x" 
                  << std::hex << hwnd << std::dec << ". Error: " << GetLastError() << "\n";
        return false;
    }

    // Ensure window is restored (not minimized or maximized)
    ShowWindow(hwnd, SW_RESTORE);

    // Remove WS_CAPTION and WS_THICKFRAME to make the window borderless
    LONG newStyle = originalStyle & ~(WS_CAPTION | WS_THICKFRAME);
    if (!SetWindowLong(hwnd, GWL_STYLE, newStyle)) {
        std::cerr << "MoveWindowNormalized: Failed to set window style for HWND=0x" 
                  << std::hex << hwnd << std::dec << ". Error: " << GetLastError() << "\n";
        return false;
    }

    // Apply the style change
    if (!SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, 
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER)) {
        std::cerr << "MoveWindowNormalized: Failed to update window style for HWND=0x" 
                  << std::hex << hwnd << std::dec << ". Error: " << GetLastError() << "\n";
        return false;
    }

    // Move the window to the specified position and size
    BOOL success = SetWindowPos(hwnd, HWND_TOP, x, y, width, height, 
        SWP_NOZORDER | SWP_SHOWWINDOW);
    if (!success) {
        std::cerr << "MoveWindowNormalized: Failed to move HWND=0x" 
                  << std::hex << hwnd << std::dec << ". Error: " << GetLastError() << "\n";
    }

    return success != FALSE;
}


// Callback to collect visible windows that will be managed
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;                   // Skip invisible windows
    if (GetWindowTextLengthA(hwnd) == 0) return TRUE;          // Skip untitled windows
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);

    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;               // Skip tool windows
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if ((style & WS_POPUP) || (style & WS_CHILD)) return TRUE; // Skip popups or child windows

    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        std::cerr << "EnumWindowsCallback: Failed to retrieve RECT for HWND 0x" 
                  << std::hex << hwnd << std::dec << ". Error: " << GetLastError() << "\n";
        return TRUE;
    }

    if (rect.left == rect.right || rect.top == rect.bottom) return TRUE; // Skip windows with no area

    // Retrieve window title
    std::string title = GetWindowTitle(hwnd);
    if (title.empty()) return TRUE; // Skip windows without titles

    // Initialize WindowInfo and add to managedWindows
    WindowInfo winInfo;
    winInfo.hwnd = hwnd;
    winInfo.savedRect = rect; // Initially set to current rect
    winInfo.savedStyle = style;
    windows->push_back(winInfo);

    // Debug: Print added window with title
    std::cout << "EnumWindowsCallback: Managed window added: HWND=0x" 
              << std::hex << hwnd << std::dec << ", Title=\"" << title << "\"\n";

    return TRUE;
}

// Function to initialize the layout with the first window
void InitializeLayout(HWND firstWindow) {
    root = std::make_unique<LayoutNode>(firstWindow);
}

// Function to add a new window using breadth-first split strategy with depth tracking
void AddWindowBreadthFirst(HWND newWindow, float splitRatio) {
    if (!root) {
        // If root is not initialized, initialize with the new window
        root = std::make_unique<LayoutNode>(newWindow);
        return;
    }

    // Use a queue to perform breadth-first traversal with depth tracking
    std::queue<QueueItem> nodeQueue;
    nodeQueue.push(QueueItem{ root.get(), 0 }); // Root node at depth 0

    while (!nodeQueue.empty()) {
        QueueItem currentItem = nodeQueue.front();
        nodeQueue.pop();

        LayoutNode* current = currentItem.node;
        int depth = currentItem.depth;

        if (!current->isSplit) {
            // Split this leaf node
            current->isSplit = true;

            // Determine split type based on depth
            SplitType splitType = (depth % 2 == 0) ? SplitType::HORIZONTAL : SplitType::VERTICAL;
            current->splitType = splitType;
            current->splitRatio = splitRatio;

            // Create child nodes
            current->firstChild = std::make_unique<LayoutNode>(current->windowInfo.hwnd);
            current->firstChild->parent = current;
            current->secondChild = std::make_unique<LayoutNode>(newWindow);
            current->secondChild->parent = current;

            // Clear the window handle in the split node
            current->windowInfo.hwnd = nullptr;

            // Enqueue child nodes with incremented depth
            nodeQueue.push(QueueItem{ current->firstChild.get(), depth + 1 });
            nodeQueue.push(QueueItem{ current->secondChild.get(), depth + 1 });

            return; // Window added successfully
        }
        else {
            // If it's a split node, enqueue its children with incremented depth
            if (current->firstChild) {
                nodeQueue.push(QueueItem{ current->firstChild.get(), depth + 1 });
            }
            if (current->secondChild) {
                nodeQueue.push(QueueItem{ current->secondChild.get(), depth + 1 });
            }
        }
    }
}

// Function to determine the split type based on direction
SplitType GetSplitTypeFromDirection(Direction dir) {
    switch (dir) {
        case Direction::LEFT:
        case Direction::RIGHT:
            return SplitType::VERTICAL;
        case Direction::UP:
        case Direction::DOWN:
            return SplitType::HORIZONTAL;
        default:
            return SplitType::VERTICAL; // Default fallback
    }
}

// Function to apply the layout by traversing the tree
void ApplyLayout(LayoutNode* node, RECT area) {
    if (!node) return;

    if (!node->isSplit) {
        // This is a leaf node; move the window to the specified area
        if (node->windowInfo.hwnd != nullptr) {
            if (MoveWindowNormalized(node->windowInfo.hwnd, area.left, area.top,
                area.right - area.left, area.bottom - area.top)) {
                node->windowRect = area; // Store the window's position
            }
        }
        return;
    }

    // Calculate the split
    if (node->splitType == SplitType::VERTICAL) {
        int splitPos = area.left + static_cast<int>((area.right - area.left) * node->splitRatio);
        RECT firstArea = { area.left, area.top, splitPos, area.bottom };
        RECT secondArea = { splitPos, area.top, area.right, area.bottom };
        ApplyLayout(node->firstChild.get(), firstArea);
        ApplyLayout(node->secondChild.get(), secondArea);
    }
    else { // SplitType::HORIZONTAL
        int splitPos = area.top + static_cast<int>((area.bottom - area.top) * node->splitRatio);
        RECT firstArea = { area.left, area.top, area.right, splitPos };
        RECT secondArea = { area.left, splitPos, area.right, area.bottom };
        ApplyLayout(node->firstChild.get(), firstArea);
        ApplyLayout(node->secondChild.get(), secondArea);
    }
}

// Function to tile all windows based on the layout tree
void TileWindows(const RECT& screenRect) {
    if (root) {
        ApplyLayout(root.get(), screenRect);
        std::cout << "TileWindows: Windows tiled successfully.\n";
    }
    else {
        std::cerr << "TileWindows: Layout root is null. No windows to tile.\n";
    }
}

// Function to toggle fullscreen for a window
void SetWindowFullscreen(LayoutNode* node, const RECT& monitorRect) {
    if (!node || node->windowInfo.hwnd == nullptr) return;

    WindowInfo& windowInfo = node->windowInfo;

    if (!windowInfo.isFullscreen) {
        // Save current window state
        windowInfo.savedStyle = GetWindowLong(node->windowInfo.hwnd, GWL_STYLE);
        if (!GetWindowRect(node->windowInfo.hwnd, &windowInfo.savedRect)) {
            std::cerr << "SetWindowFullscreen: Failed to get window rect for HWND=0x" 
                      << std::hex << node->windowInfo.hwnd << std::dec 
                      << ". Error: " << GetLastError() << "\n";
            return;
        }

        // Remove borders, title bar, etc.
        SetWindowLong(node->windowInfo.hwnd, GWL_STYLE, windowInfo.savedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU));

        // Resize and reposition to cover the entire screen
        MoveWindowNormalized(node->windowInfo.hwnd,
            monitorRect.left,
            monitorRect.top,
            monitorRect.right - monitorRect.left,
            monitorRect.bottom - monitorRect.top);
    }
    else {
        // Restore original window style
        SetWindowLong(node->windowInfo.hwnd, GWL_STYLE, windowInfo.savedStyle);
        SetWindowPos(node->windowInfo.hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

        // Restore original window size and position
        MoveWindowNormalized(node->windowInfo.hwnd,
            windowInfo.savedRect.left,
            windowInfo.savedRect.top,
            windowInfo.savedRect.right - windowInfo.savedRect.left,
            windowInfo.savedRect.bottom - windowInfo.savedRect.top);
    }

    // Toggle the fullscreen flag
    windowInfo.isFullscreen = !windowInfo.isFullscreen;

    // Force redraw
    ShowWindow(node->windowInfo.hwnd, SW_SHOW);
}

// Function to find a LayoutNode given an HWND
LayoutNode* FindLayoutNode(LayoutNode* node, HWND hwnd) {
    if (!node) return nullptr;
    if (!node->isSplit && node->windowInfo.hwnd == hwnd) return node;
    if (node->isSplit) {
        LayoutNode* found = FindLayoutNode(node->firstChild.get(), hwnd);
        if (found) return found;
        return FindLayoutNode(node->secondChild.get(), hwnd);
    }
    return nullptr;
}

// Function to collect all leaf nodes
void CollectLeafNodes(LayoutNode* node, std::vector<LayoutNode*>& leaves) {
    if (!node) return;
    if (!node->isSplit && node->windowInfo.hwnd != nullptr) {
        leaves.push_back(node);
        return;
    }
    if (node->isSplit) {
        CollectLeafNodes(node->firstChild.get(), leaves);
        CollectLeafNodes(node->secondChild.get(), leaves);
    }
}

// Function to swap two window handles
bool SwapWindowHandles(LayoutNode* nodeA, LayoutNode* nodeB) {
    if (!nodeA || !nodeB) return false;
    if (nodeA->windowInfo.hwnd == nullptr || nodeB->windowInfo.hwnd == nullptr) return false;

    std::swap(nodeA->windowInfo.hwnd, nodeB->windowInfo.hwnd);

    std::cout << "SwapWindowHandles: Swapped window handles between HWND 0x" 
              << std::hex << nodeA->windowInfo.hwnd << " and HWND 0x" 
              << nodeB->windowInfo.hwnd << std::dec << ".\n";

    // Reapply the layout to update window positions
    HDC hdcScreen = GetDC(nullptr);
    RECT screenRect;
    screenRect.left = 0;
    screenRect.top = 0;
    screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
    screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(nullptr, hdcScreen);

    TileWindows(screenRect);

    return true;
}

void OutlineWindow(HWND hwnd, unsigned int hexColor, int thickness) {
    // Get the dimensions of the window
    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        return; // Failed to get the window rect
    }

    // Get the device context of the desktop (where the window is drawn)
    HDC hdc = GetDC(nullptr);
    if (!hdc) {
        return; // Failed to get the device context
    }

    // Convert hexColor to COLORREF
    COLORREF color = RGB((hexColor >> 16) & 0xFF, (hexColor >> 8) & 0xFF, hexColor & 0xFF);

    // Create a solid brush with the desired color
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) {
        ReleaseDC(nullptr, hdc);
        return; // Failed to create the brush
    }

    // Draw the border
    for (int i = 0; i < thickness; ++i) {
        RECT borderRect = {
            rect.left - i,
            rect.top - i,
            rect.right + i,
            rect.bottom + i
        };
        FrameRect(hdc, &borderRect, brush);
    }

    // Clean up
    DeleteObject(brush);
    ReleaseDC(nullptr, hdc);
}



void CreateOverlayWindow(HWND targetHwnd) {
    if (g_hOverlay != NULL) return; // Overlay already exists

    // Register window class
    const char CLASS_NAME[] = "OverlayWindowClass"; // Use narrow string

    WNDCLASSA wc = { }; // Use WNDCLASSA for ANSI
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClassA(&wc)) { // Use RegisterClassA
        std::cerr << "Failed to register window class.\n";
        return;
    }

    // Get target window position and size
    RECT rect;
    GetWindowRect(targetHwnd, &rect);

    // Create the overlay window
    g_hOverlay = CreateWindowExA( // Use CreateWindowExA
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        CLASS_NAME,
        NULL,
        WS_POPUP,
        rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL,
        NULL,
        GetModuleHandle(NULL),
        NULL
    );

    if (!g_hOverlay) {
        std::cerr << "Failed to create overlay window.\n";
        return;
    }

    // Make the window transparent
    // Make pure blue (RGB 0, 0, 255) transparent
SetLayeredWindowAttributes(g_hOverlay, RGB(255, 255, 255), 0, LWA_COLORKEY);


    ShowWindow(g_hOverlay, SW_SHOW);
}

void UpdateOverlayWindow(HWND targetHwnd) {
    if (g_hOverlay == NULL) return;

    // Get target window position and size
    RECT rect;
    GetWindowRect(targetHwnd, &rect);

    // Move and resize the overlay window to match the target window
    SetWindowPos(g_hOverlay, HWND_TOPMOST, rect.left, rect.top, 
                 rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE);
}

void DestroyOverlayWindow() {
    if (g_hOverlay != NULL) {
        DestroyWindow(g_hOverlay);
        g_hOverlay = NULL;
    }
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Create a pen for the border
        HPEN hPen = CreatePen(PS_SOLID, g_BorderThickness, g_BorderColor);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

        // Get client area
        RECT rect;
        GetClientRect(hwnd, &rect);

        // Draw the border
        for (int i = 0; i < g_BorderThickness; ++i) {
            Rectangle(hdc, rect.left + i, rect.top + i, rect.right - i, rect.bottom - i);
        }

        // Cleanup
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void FocusWindow(LayoutNode* node) {
    if (!node || node->windowInfo.hwnd == nullptr) return;

    HWND hwnd = node->windowInfo.hwnd;

    std::string title = GetWindowTitle(hwnd);
    std::cout << "FocusWindow: Focusing window: " << title << " (HWND=0x" 
              << std::hex << reinterpret_cast<uintptr_t>(hwnd) << std::dec << ")\n";

    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, 
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);

    // Create and update the overlay window
    CreateOverlayWindow(hwnd);
    UpdateOverlayWindow(hwnd);

    // Optionally, handle window move/resize events to update the overlay
    // This requires additional implementation, such as setting up a hook or a timer
}

// Function to register hotkeys
bool RegisterHotKeys() {
    bool success = true;

    // Lambda that attempts hotkey registration and reports failures
    auto register_hotkey = [&](int id, UINT modifiers, UINT vk, const char* description) -> bool {
        if (!RegisterHotKey(nullptr, id, modifiers, vk)) {
            std::cerr << "RegisterHotKeys: Failed to register hotkey ID " << id 
                      << " (" << description << "). Error: " << GetLastError() << "\n";
            return false;
        }
        return true;
    };

    // Register navigation and focus hotkeys
    success &= register_hotkey(1, MOD_KEY, VK_LEFT, "Focus Left");
    success &= register_hotkey(2, MOD_KEY, VK_RIGHT, "Focus Right");
    success &= register_hotkey(6, MOD_KEY, VK_UP, "Focus Up");
    success &= register_hotkey(7, MOD_KEY, VK_DOWN, "Focus Down");

    // Register move hotkeys
    success &= register_hotkey(11, MOD_KEY | MOD_SHIFT, VK_UP, "Move Up");
    success &= register_hotkey(12, MOD_KEY | MOD_SHIFT, VK_DOWN, "Move Down");
    success &= register_hotkey(13, MOD_KEY | MOD_SHIFT, VK_LEFT, "Move Left");
    success &= register_hotkey(14, MOD_KEY | MOD_SHIFT, VK_RIGHT, "Move Right");

    // Register Close Current Window hotkey
    success &= register_hotkey(15, MOD_KEY | MOD_SHIFT, 'Q', "Close Current Window");

    // Register fullscreen toggle hotkey
    success &= register_hotkey(3, MOD_KEY, 'F', "Toggle Fullscreen");

    // Register resize mode toggle hotkey
    success &= register_hotkey(10, MOD_KEY, 'R', "Toggle Resize Mode");

    // **Register Split Orientation Toggle Hotkeys**
    success &= register_hotkey(16, MOD_KEY, 'V', "Toggle to Vertical Split");
    success &= register_hotkey(17, MOD_KEY, 'H', "Toggle to Horizontal Split");

    return success;
}

// Function to unregister all hotkeys
void UnregisterHotKeys() {
    for (int id = 1; id <= 17; ++id) { // Updated to 17 to include new hotkeys
        UnregisterHotKey(nullptr, id);
    }
    std::cout << "UnregisterHotKeys: All hotkeys unregistered.\n";
}

// Function to find the adjacent LayoutNode in a given direction
LayoutNode* FindAdjacent(LayoutNode* current, Direction dir) {
    if (!current) return nullptr;

    LayoutNode* node = current;
    LayoutNode* parent = node->parent;

    // Determine the required split type based on direction
    SplitType requiredSplit;
    switch (dir) {
        case Direction::LEFT:
        case Direction::RIGHT:
            requiredSplit = SplitType::VERTICAL;
            break;
        case Direction::UP:
        case Direction::DOWN:
            requiredSplit = SplitType::HORIZONTAL;
            break;
    }

    // Traverse up to find the nearest ancestor split matching the required split
    while (parent) {
        if (parent->isSplit && parent->splitType == requiredSplit) {
            // Determine if current node is firstChild or secondChild
            bool isFirst = (parent->firstChild.get() == node);

            // Determine the direction to navigate
            bool navigateToSecond = false;
            if ((dir == Direction::LEFT && !isFirst) ||
                (dir == Direction::RIGHT && isFirst) ||
                (dir == Direction::UP && !isFirst) ||
                (dir == Direction::DOWN && isFirst)) {
                navigateToSecond = true;
            }

            if (navigateToSecond) {
                // Traverse the sibling subtree to find the target window
                LayoutNode* sibling = isFirst ? parent->secondChild.get() : parent->firstChild.get();

                // Depending on the direction, traverse to the appropriate window
                std::function<LayoutNode*(LayoutNode*)> findTarget;
                if (dir == Direction::LEFT || dir == Direction::UP) {
                    // For LEFT and UP, find the rightmost or bottommost window
                    findTarget = [&](LayoutNode* n) -> LayoutNode* {
                        if (!n->isSplit) return n;
                        // For LEFT and UP, go to the second child
                        return findTarget(n->secondChild.get());
                    };
                }
                else { // RIGHT and DOWN
                    // For RIGHT and DOWN, find the leftmost or topmost window
                    findTarget = [&](LayoutNode* n) -> LayoutNode* {
                        if (!n->isSplit) return n;
                        // For RIGHT and DOWN, go to the first child
                        return findTarget(n->firstChild.get());
                    };
                }

                LayoutNode* target = findTarget(sibling);
                // Ensure that we are not selecting the same node
                if (target->windowInfo.hwnd != current->windowInfo.hwnd) {
                    return target;
                }
            }
        }
        node = parent;
        parent = node->parent;
    }

    // No adjacent found
    return nullptr;
}

// Function to navigate in a given direction
void Navigate(Direction dir) {
    HWND current = GetForegroundWindow();
    LayoutNode* currentNode = FindLayoutNode(root.get(), current);
    if (currentNode) {
        LayoutNode* adjacent = FindAdjacent(currentNode, dir);
        if (adjacent && adjacent->windowInfo.hwnd != nullptr) {
            FocusWindow(adjacent);
        }
        else {
            std::cout << "Navigate: No window in the " <<
                (dir == Direction::LEFT ? "LEFT" :
                 dir == Direction::RIGHT ? "RIGHT" :
                 dir == Direction::UP ? "UP" : "DOWN") << " direction.\n";
        }
    }
    else {
        std::cerr << "Navigate: Current window not managed.\n";
    }
}

// Function to adjust splitRatio and reapply layout
void AdjustSplitRatio(LayoutNode* node, float deltaRatio) {
    if (!node || !node->isSplit) return;

    // Adjust the split ratio
    node->splitRatio += deltaRatio;

    // Clamp the split ratio to avoid extreme sizes
    // NOTE: minwindef.h which is included indirectly, defines a min and max method. I've wrapped
    // std::min and std::max here with parenthesis to fully qualify their names and prevent warnings
    node->splitRatio = (std::max)(0.2f, (std::min)(0.8f, node->splitRatio));

    // Re-apply the layout
    HDC hdcScreen = GetDC(nullptr);
    RECT screenRect;
    screenRect.left = 0;
    screenRect.top = 0;
    screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
    screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(nullptr, hdcScreen);

    TileWindows(screenRect);
}

// Function to move a window in a given direction
bool MoveWindowInDirection(Direction dir) {
    std::lock_guard<std::mutex> lock(layoutMutex); // Ensure thread safety

    HWND current = GetForegroundWindow();
    LayoutNode* currentNode = FindLayoutNode(root.get(), current);
    if (!currentNode) {
        std::cerr << "MoveWindowInDirection: Current window not managed.\n";
        return false;
    }

    // Debug: Print layout before moving
    std::cout << "MoveWindowInDirection: Layout before moving:\n";
    PrintLayout(root.get());

    LayoutNode* adjacentNode = FindAdjacent(currentNode, dir);
    if (!adjacentNode) {
        std::cout << "MoveWindowInDirection: No window in the " <<
            (dir == Direction::LEFT ? "LEFT" :
             dir == Direction::RIGHT ? "RIGHT" :
             dir == Direction::UP ? "UP" : "DOWN") << " direction to move.\n";

        // **Prevent Split Creation Without Window Assignment**
        // Check if there's a pending split available
        if (!pendingSplits.empty()) {
            std::cout << "MoveWindowInDirection: Pending splits exist. Waiting for window assignment.\n";
            return false; // Do not create a new split
        }

        // Determine if split orientation needs to change
        SplitType desiredSplit = GetSplitTypeFromDirection(dir);
        if (currentNode->parent && currentNode->parent->splitType != desiredSplit) {
            // Change split type of the parent
            currentNode->parent->splitType = desiredSplit;
            std::cout << "MoveWindowInDirection: Changed split type to " <<
                (desiredSplit == SplitType::VERTICAL ? "VERTICAL" : "HORIZONTAL") << ".\n";

            // Reapply layout to adjust window positions
            HDC hdcScreen = GetDC(nullptr);
            RECT screenRect;
            screenRect.left = 0;
            screenRect.top = 0;
            screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
            screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
            ReleaseDC(nullptr, hdcScreen);

            TileWindows(screenRect);
            return true;
        }

        // If split orientation does not need to change, do not alter layout
        std::cout << "MoveWindowInDirection: Split orientation does not need to change.\n";
        return false; // Exit without altering the layout
    }

    // Debug: Print adjacent window
    std::cout << "MoveWindowInDirection: Adjacent window HWND=0x" 
              << std::hex << adjacentNode->windowInfo.hwnd << std::dec << "\n";

    // Swap the window handles
    if (SwapWindowHandles(currentNode, adjacentNode)) {
        std::cout << "MoveWindowInDirection: Swapped windows successfully.\n";
        // Debug: Print layout after moving
        std::cout << "MoveWindowInDirection: Layout after moving:\n";
        PrintLayout(root.get());
        return true;
    }

    return false;
}

// Function to change the split orientation of the current container
void ChangeSplitOrientation(SplitType newSplitType) {
    std::lock_guard<std::mutex> lock(layoutMutex);

    HWND current = GetForegroundWindow();
    LayoutNode* currentNode = FindLayoutNode(root.get(), current);

    if (!currentNode) {
        std::cerr << "ChangeSplitOrientation: Current window not managed.\n";
        return;
    }

    // Find the parent split node
    LayoutNode* parent = currentNode->parent;
    if (!parent) {
        std::cerr << "ChangeSplitOrientation: Current window has no parent split node.\n";
        return;
    }

    // Check if the parent split type is already the desired type
    if (parent->splitType == newSplitType) {
        std::cout << "ChangeSplitOrientation: Split type is already " 
                  << (newSplitType == SplitType::VERTICAL ? "Vertical" : "Horizontal") << ".\n";
        return;
    }

    // Change the split type
    parent->splitType = newSplitType;
    std::cout << "ChangeSplitOrientation: Split type changed to " 
              << (newSplitType == SplitType::VERTICAL ? "Vertical" : "Horizontal") << ".\n";

    // Re-apply the layout to reflect the change
    HDC hdcScreen = GetDC(nullptr);
    RECT screenRect;
    screenRect.left = 0;
    screenRect.top = 0;
    screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
    screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(nullptr, hdcScreen);

    TileWindows(screenRect);
}

// Debugging Function to Print the Layout Tree
void PrintLayout(LayoutNode* node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth; ++i) std::cout << "  ";
    if (node->isSplit) {
        std::cout << "Split: " << (node->splitType == SplitType::VERTICAL ? "Vertical" : "Horizontal") 
                  << ", Ratio: " << node->splitRatio << "\n";
        PrintLayout(node->firstChild.get(), depth + 1);
        PrintLayout(node->secondChild.get(), depth + 1);
    }
    else {
        std::string title = GetWindowTitle(node->windowInfo.hwnd);
        std::cout << "Window: HWND=0x" << std::hex << node->windowInfo.hwnd << std::dec 
                  << ", Title=\"" << title << "\"\n";
    }
}

// Low-level keyboard hook callback
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && isResizeMode && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        // Determine if Shift is pressed
        bool isShiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

        // Determine delta ratio based on key and split type
        float deltaRatio = 0.0f;
        if (activeNodeForResize && activeNodeForResize->parent) {
            LayoutNode* parentSplitNode = activeNodeForResize->parent;
            switch (p->vkCode) {
                case VK_LEFT:
                    if (parentSplitNode->splitType == SplitType::VERTICAL) {
                        deltaRatio = isShiftPressed ? -0.02f : 0.02f;
                    }
                    break;
                case VK_RIGHT:
                    if (parentSplitNode->splitType == SplitType::VERTICAL) {
                        deltaRatio = isShiftPressed ? 0.02f : -0.02f;
                    }
                    break;
                case VK_UP:
                    if (parentSplitNode->splitType == SplitType::HORIZONTAL) {
                        deltaRatio = isShiftPressed ? -0.02f : 0.02f;
                    }
                    break;
                case VK_DOWN:
                    if (parentSplitNode->splitType == SplitType::HORIZONTAL) {
                        deltaRatio = isShiftPressed ? 0.02f : -0.02f;
                    }
                    break;
                case VK_ESCAPE:
                    // Exit resize mode on ESC
                    isResizeMode = false;
                    if (hKeyboardHook) {
                        UnhookWindowsHookEx(hKeyboardHook);
                        hKeyboardHook = NULL;
                    }
                    std::cout << "LowLevelKeyboardProc: Exited resize mode.\n";
                    return 1; // Suppress the key
                default:
                    break;
            }

            if (deltaRatio != 0.0f) {
                // Adjust the split ratio
                AdjustSplitRatio(parentSplitNode, deltaRatio);
            }
        }

        return 1; // Suppress the key
    }
    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

// WinEvent callback implementation
void CALLBACK WinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD dwEventThread,
    DWORD dwmsEventTime
) {
    // Log every event received with HWND in hexadecimal
    std::cout << "WinEventProc: Event " << event << " received for HWND=0x" 
              << std::hex << hwnd << std::dec << "\n";

    // Only process window-level events
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }

    std::lock_guard<std::mutex> lock(layoutMutex); // Lock the mutex

    // Function to process window addition
    auto processWindow = [&](HWND hwnd) {
        std::cout << "Processing window: HWND=0x" << std::hex << hwnd << std::dec << "\n";

        if (!IsWindowVisible(hwnd)) {
            std::cout << " - Skipped: Window is not visible.\n";
            return;
        }

        int titleLength = GetWindowTextLengthA(hwnd);
        if (titleLength == 0) {
            std::cout << " - Skipped: Window has no title.\n";
            return;
        }

        // Retrieve window title
        std::string title = GetWindowTitle(hwnd); // Using helper function

        LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) {
            std::cout << " - Skipped: Window is a tool window. Title=\"" << title << "\"\n";
            return;
        }

        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        if ((style & WS_POPUP) || (style & WS_CHILD)) {
            std::cout << " - Skipped: Window is a popup or child window. Title=\"" << title << "\"\n";
            return;
        }

        RECT rect;
        if (!GetWindowRect(hwnd, &rect)) {
            std::cerr << " - Error: Failed to get RECT for HWND=0x" << std::hex << hwnd 
                      << ". Error: " << GetLastError() << "\n" << std::dec;
            return;
        }

        if (rect.left == rect.right || rect.top == rect.bottom) {
            std::cout << " - Skipped: Window has no area. Title=\"" << title << "\"\n";
            return;
        }

        // Initialize WindowInfo
        WindowInfo winInfo;
        winInfo.hwnd = hwnd;
        winInfo.savedRect = rect;
        winInfo.savedStyle = style;
        managedWindows.push_back(winInfo);
        std::cout << " - Added: New window managed. Title=\"" << title << "\"\n";

        // Assign the new window to the first available pending split
        bool assigned = false;
        while (!pendingSplits.empty()) {
            LayoutNode* pendingNode = pendingSplits.front();
            pendingSplits.pop();

            if (pendingNode && !pendingNode->isSplit && pendingNode->windowInfo.hwnd == nullptr) {
                pendingNode->windowInfo.hwnd = hwnd;
                std::cout << " - Assigned new window to pending split.\n";
                assigned = true;
                break;
            }
        }

        if (!assigned) {
            // If no pending split found, add breadth-first
            std::cout << " - No pending split found. Adding breadth-first.\n";
            AddWindowBreadthFirst(hwnd);
        }

        // Re-apply the tiling layout
        HDC hdcScreen = GetDC(nullptr);
        RECT screenRect;
        screenRect.left = 0;
        screenRect.top = 0;
        screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
        screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
        ReleaseDC(nullptr, hdcScreen);

        TileWindows(screenRect);
    };

    // Handle window show events
    if (event == EVENT_OBJECT_SHOW) {
        processWindow(hwnd);
    }
    // Handle window destruction
    else if (event == EVENT_OBJECT_DESTROY) {
        // Find and remove the window from managedWindows and the layout tree
        auto it = std::find_if(managedWindows.begin(), managedWindows.end(),
            [hwnd](const WindowInfo& win) { return win.hwnd == hwnd; });
        if (it != managedWindows.end()) {
            std::cout << "WinEventProc: Window removed: HWND=0x" << std::hex << hwnd << std::dec << "\n";
            managedWindows.erase(it);

            // Find the corresponding LayoutNode
            LayoutNode* nodeToRemove = FindLayoutNode(root.get(), hwnd);
            if (nodeToRemove) {
                // Remove the node from the layout tree
                if (nodeToRemove->parent) {
                    LayoutNode* parent = nodeToRemove->parent;
                    std::unique_ptr<LayoutNode> sibling;
                    if (parent->firstChild.get() == nodeToRemove) {
                        sibling = std::move(parent->secondChild);
                    } else {
                        sibling = std::move(parent->firstChild);
                    }

                    // Replace parent with sibling
                    if (parent->parent) {
                        if (parent->parent->firstChild.get() == parent) {
                            parent->parent->firstChild = std::move(sibling);
                            parent->parent->firstChild->parent = parent->parent;
                        } else {
                            parent->parent->secondChild = std::move(sibling);
                            parent->parent->secondChild->parent = parent->parent;
                        }
                    }
                    else {
                        // If parent is root
                        root = std::move(sibling);
                        if (root) {
                            root->parent = nullptr;
                        }
                    }
                }
                else {
                    // If the node to remove is root
                    root.reset();
                }

                // Re-apply the tiling layout
                HDC hdcScreen = GetDC(nullptr);
                RECT screenRect;
                screenRect.left = 0;
                screenRect.top = 0;
                screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
                screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
                ReleaseDC(nullptr, hdcScreen);

                TileWindows(screenRect);
            }
        }
    }
}

// Function to unregister all WinEvent hooks
void UnregisterWinEventHooks(HWINEVENTHOOK hHookShow, HWINEVENTHOOK hHookDestroy, HWINEVENTHOOK hHookNameChange = nullptr) {
    if (hHookShow) {
        UnhookWinEvent(hHookShow);
    }
    if (hHookDestroy) {
        UnhookWinEvent(hHookDestroy);
    }
    if (hHookNameChange) {
        UnhookWinEvent(hHookNameChange);
    }
    std::cout << "UnregisterWinEventHooks: All WinEvent hooks unregistered.\n";
}

// Function to close a focused window
void CloseFocusedWindow(HWND currentWindow) {
    PostMessage(currentWindow, WM_CLOSE, 0, 0);
}

int main() {
    // Ensure the program is DPI Aware using SetProcessDPIAware
    BOOL dpiResult = SetProcessDPIAware();
    if (!dpiResult) {
        std::cerr << "Failed to set DPI awareness. Error: " << GetLastError() << "\n";
    } else {
        std::cout << "DPI awareness set successfully.\n";
    }

    // Enumerate all visible windows
    std::cout << "Main: Enumerating windows...\n";
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&managedWindows));

    if (managedWindows.empty()) {
        std::cerr << "Main: No windows to manage.\n";
        return 1;
    }

    // Get screen dimensions
    HDC hdcScreen = GetDC(nullptr);
    RECT screenRect;
    screenRect.left = 0;
    screenRect.top = 0;
    screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
    screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(nullptr, hdcScreen);

    std::cout << "Main: Screen dimensions: Width=" << screenRect.right
              << ", Height=" << screenRect.bottom << "\n";

    // Initialize the layout with the first window
    InitializeLayout(managedWindows[0].hwnd);

    // Add remaining windows to the layout
    for (size_t i = 1; i < managedWindows.size(); ++i) {
        AddWindowBreadthFirst(managedWindows[i].hwnd);
    }

    // Apply the tiling layout and store window positions
    TileWindows(screenRect);

    // Register hotkeys for switching, moving, and other functionalities
    if (!RegisterHotKeys()) {
        std::cerr << "Main: Failed to register hotkeys.\n";
        UnregisterHotKeys(); // Clean up any successfully registered hotkeys
        return 1;
    }

    std::cout << "Main: Hotkeys registered successfully.\n";
    std::cout << "Main: Available Hotkeys:\n";
    std::cout << "  MOD + LEFT/RIGHT: Focus adjacent windows horizontally.\n";
    std::cout << "  MOD + UP/DOWN: Focus adjacent windows vertically.\n";
    std::cout << "  MOD + SHIFT + LEFT/RIGHT/UP/DOWN: Move focused window in the specified direction.\n";
    std::cout << "  MOD + F: Toggle fullscreen on the active window.\n";
    std::cout << "  MOD + V: Toggle to Vertical Split of the current container.\n";    // New Hotkey
    std::cout << "  MOD + H: Toggle to Horizontal Split of the current container.\n";  // New Hotkey
    std::cout << "  MOD + R: Toggle resize mode.\n";
    std::cout << "    While in resize mode, use arrow keys to resize the focused window.\n";
    std::cout << "      Press SHIFT + Arrow Key to shrink the window.\n";
    std::cout << "      Press Arrow Key alone to grow the window.\n";
    std::cout << "    Press ESC or MOD + R to exit resize mode.\n";
    std::cout << "  MOD + SHIFT + Q: Close the focused window.\n";

    // Register WinEvent hooks for window show and destruction
    HWINEVENTHOOK hEventHookShow = SetWinEventHook(
        EVENT_OBJECT_SHOW,      // EventMin
        EVENT_OBJECT_SHOW,      // EventMax
        nullptr,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    HWINEVENTHOOK hEventHookDestroy = SetWinEventHook(
        EVENT_OBJECT_DESTROY,
        EVENT_OBJECT_DESTROY,
        nullptr,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    if (!hEventHookShow || !hEventHookDestroy) {
        std::cerr << "Main: Failed to set WinEvent hooks. Error: " << GetLastError() << "\n";
    } else {
        std::cout << "Main: WinEvent hooks for show and destruction set successfully.\n";
    }

    // Message loop to handle hotkey and window events
    MSG msg = { 0 };
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            switch (msg.wParam) {
                case 1: { // MOD + LEFT
                    std::cout << "Hotkey 1: MOD + LEFT pressed. Focusing left window.\n";
                    Navigate(Direction::LEFT);
                    break;
                }
                case 2: { // MOD + RIGHT
                    std::cout << "Hotkey 2: MOD + RIGHT pressed. Focusing right window.\n";
                    Navigate(Direction::RIGHT);
                    break;
                }
                case 3: { // MOD + F (Toggle Fullscreen)
                    std::cout << "Hotkey 3: MOD + F pressed. Toggling fullscreen.\n";
                    HWND current = GetForegroundWindow();
                    LayoutNode* currentNode = FindLayoutNode(root.get(), current);
                    if (currentNode && currentNode->windowInfo.hwnd != nullptr) {
                        // Get monitor information for fullscreen
                        HMONITOR hMonitor = MonitorFromWindow(currentNode->windowInfo.hwnd, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
                        if (!GetMonitorInfo(hMonitor, &monitorInfo)) {
                            std::cerr << "Hotkey 3: Failed to get monitor info. Error: " << GetLastError() << "\n";
                            break;
                        }

                        // Toggle fullscreen
                        SetWindowFullscreen(currentNode, monitorInfo.rcMonitor);
                    }
                    else {
                        std::cerr << "Hotkey 3: Current window not managed.\n";
                    }
                    break;
                }
                case 6: { // MOD + UP
                    std::cout << "Hotkey 6: MOD + UP pressed. Focusing up window.\n";
                    Navigate(Direction::UP);
                    break;
                }
                case 7: { // MOD + DOWN
                    std::cout << "Hotkey 7: MOD + DOWN pressed. Focusing down window.\n";
                    Navigate(Direction::DOWN);
                    break;
                }
                case 10: { // MOD + R (Toggle Resize Mode)
                    std::cout << "Hotkey 10: MOD + R pressed. Toggling resize mode.\n";
                    isResizeMode = !isResizeMode;
                    if (isResizeMode) {
                        // Get the currently focused window
                        HWND current = GetForegroundWindow();
                        activeNodeForResize = FindLayoutNode(root.get(), current);
                        if (!activeNodeForResize) {
                            std::cerr << "Hotkey 10: Current window not managed.\n";
                            isResizeMode = false;
                            break;
                        }

                        // Install the keyboard hook
                        hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
                        if (!hKeyboardHook) {
                            std::cerr << "Hotkey 10: Failed to install keyboard hook. Error: " << GetLastError() << "\n";
                            isResizeMode = false;
                            break;
                        }

                        std::cout << "Hotkey 10: Entered resize mode. Use arrow keys to resize.\n";
                        std::cout << "  Press SHIFT + Arrow Key to shrink the window.\n";
                        std::cout << "  Press Arrow Key alone to grow the window.\n";
                        std::cout << "  Press ESC or MOD + R to exit resize mode.\n";
                    }
                    else {
                        // Uninstall the keyboard hook
                        if (hKeyboardHook) {
                            UnhookWindowsHookEx(hKeyboardHook);
                            hKeyboardHook = NULL;
                        }
                        std::cout << "Hotkey 10: Exited resize mode.\n";
                    }
                    break;
                }
                case 11: { // MOD + SHIFT + UP (Move Up)
                    std::cout << "Hotkey 11: MOD + SHIFT + UP pressed. Moving window up.\n";
                    if (MoveWindowInDirection(Direction::UP)) {
                        std::cout << "Hotkey 11: Moved window up successfully.\n";
                    }
                    break;
                }
                case 12: { // MOD + SHIFT + DOWN (Move Down)
                    std::cout << "Hotkey 12: MOD + SHIFT + DOWN pressed. Moving window down.\n";
                    if (MoveWindowInDirection(Direction::DOWN)) {
                        std::cout << "Hotkey 12: Moved window down successfully.\n";
                    }
                    break;
                }
                case 13: { // MOD + SHIFT + LEFT (Move Left)
                    std::cout << "Hotkey 13: MOD + SHIFT + LEFT pressed. Moving window left.\n";
                    if (MoveWindowInDirection(Direction::LEFT)) {
                        std::cout << "Hotkey 13: Moved window left successfully.\n";
                    }
                    break;
                }
                case 14: { // MOD + SHIFT + RIGHT (Move Right)
                    std::cout << "Hotkey 14: MOD + SHIFT + RIGHT pressed. Moving window right.\n";
                    if (MoveWindowInDirection(Direction::RIGHT)) {
                        std::cout << "Hotkey 14: Moved window right successfully.\n";
                    }
                    break;
                }
                case 15: { // MOD + SHIFT + Q (Close Focused Window)
                    std::cout << "Hotkey 15: MOD + SHIFT + Q pressed. Closing Focused Window.\n";
                    HWND current = GetForegroundWindow();
                    CloseFocusedWindow(current);
                    break;
                }
                case 16: { // MOD + V (Toggle to Vertical Split)
                    std::cout << "Hotkey 16: MOD + V pressed. Changing split to Vertical.\n";
                    ChangeSplitOrientation(SplitType::VERTICAL);
                    break;
                }
                case 17: { // MOD + H (Toggle to Horizontal Split)
                    std::cout << "Hotkey 17: MOD + H pressed. Changing split to Horizontal.\n";
                    ChangeSplitOrientation(SplitType::HORIZONTAL);
                    break;
                }
                default:
                    std::cerr << "Main: Unknown hotkey ID received: " << msg.wParam << "\n";
                    break;
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Ensure the keyboard hook is removed before exiting
    if (hKeyboardHook) {
        UnhookWindowsHookEx(hKeyboardHook);
        hKeyboardHook = NULL;
    }

    // Unregister all hotkeys before exiting
    UnregisterHotKeys();

    // Unhook WinEvent hooks
    UnregisterWinEventHooks(hEventHookShow, hEventHookDestroy, nullptr);

    std::cout << "Main: Application exiting.\n";
    return 0;
}
