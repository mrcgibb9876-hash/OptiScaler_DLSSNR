#include "pch.h"

#include <Config.h>
#include <Logger.h>
#include <resource.h>
#include <Util.h>

#include "menu_common.h"
#include "menu_dx_base.h"

#include <imgui/imgui_impl_win32.h>

bool MenuDxBase::RenderMenu()
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return false;

    if (MenuCommon::RenderMenu())
    {
        ImGui::Render();
        return true;
    }

    return false;
}

bool MenuDxBase::IsHandleDifferent()
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return false;

    HWND frontWindow = Util::GetProcessWindow();

    if (frontWindow != nullptr && frontWindow == _handle)
        return false;

    // No window found this frame is not a reason to throw the overlay away. Saying "different"
    // here tears it down and takes the ImGui context with it, and the handle we already hold is
    // still the right one -- so keep it and try again next frame. Previously a single failed
    // lookup, which happens while a game is changing display mode, was enough to lose the menu
    // and the keyboard for the rest of the session.
    if (frontWindow == nullptr)
        return false;

    _handle = frontWindow;

    return true;
}

void MenuDxBase::Dx11Ready() { MenuCommon::Dx11Inited(); }

void MenuDxBase::Dx12Ready() { MenuCommon::Dx12Inited(); }

MenuDxBase::MenuDxBase(HWND handle) : _handle(handle)
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return;

    MenuCommon::Init(handle, false);

    _baseInit = MenuCommon::IsInited();
}

MenuDxBase::~MenuDxBase()
{
    if (!_baseInit)
        return;

    MenuCommon::Shutdown();
}

bool MenuDxBase::IsVisible() { return MenuCommon::IsVisible(); }
