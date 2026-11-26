using System.Numerics;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    // ---- Pages ----

    // The navigation's items name their pages (Tag): the bar's parts sit under it, numbered after the others.
    private void OnNavChanged(object sender, SelectionChangedEventArgs e)
    {
        if (Nav.SelectedItem is ListViewItem { Tag: string tag } && int.TryParse(tag, out int page)) ShowPage(page);
    }

    private void SelectPage(int page)
    {
        foreach (object? item in Nav.Items)
            if (item is ListViewItem { Tag: string tag } li && tag == page.ToString()) { Nav.SelectedItem = li; return; }
    }

    // A page is its workspace part (under the title, with the live preview on the first three) and its
    // inspector part; pages without parameters of their own (system, design, about) take the full width.
    private void ShowPage(int index)
    {
        if (_page == 8 && index != 8) StopDesignAnimations();
        _page = index;
        for (int i = 0; i < _mains.Length; i++)
        {
            _mains[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
            if (_sides[i] is { } side) side.Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_mains[index] == DesignStack)
        {
            BuildDesignPage();
            RefreshDesignDetails(); // the "current" column follows the settings
        }
        if (_mains[index] == MainWidgets)
        {
            if (WidgetGallery.Children.Count == 0) BuildWidgetGallery(); // built when first shown
            else RefreshGallery();
            ShowWidgetMap();
        }
        if (_mains[index] == MainNotify) ShowNotifyPreview(true);
        if (_mains[index] == MainStatus) BuildModuleGallery(); // follows the settings each time it shows
        if (_mains[index] == MainStage) ShowStagePreview(true);
        if (_mains[index] == MainDisplays) RefreshDisplays();
        if (index >= 10) EnterModulePage(index);
        else LeaveModulePages();
        PageTitle.Text = PageTitles[index];
        ContinuousCorners.Install(_mains[index]);
        HeroBlock.Visibility = index < 3 ? Visibility.Visible : Visibility.Collapsed;
        LoupeToggle.Visibility = index == 0 ? Visibility.Visible : Visibility.Collapsed;
        Loupe.Visibility = index == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        // The design page reads wider than a settings page.
        CenterStack.MaxWidth = _mains[index] == DesignStack ? 1280 : 1120;
        ArrangeInspector();
        // Pages and the inspector are created/reparented lazily; translate the
        // one being shown now, so a language switch does not wait for a restart.
        Loc.Localize(_mains[index]);
        if (_sides[index] is { } pageSide) Loc.Localize(pageSide);
        Loc.Localize(InspectorStack);
        Loc.Localize(FoldedInspector);
        CenterScroll.ChangeView(null, 0, null, true);
        InspectorScroll.ChangeView(null, 0, null, true);
        if (index < 3) UpdateHero();
        if (index == 0) UpdateCustomSwatch();
        AnimateIn(_mains[index]);
        if (_sides[index] is { } shown) AnimateIn(shown);
    }

    // The inspector: its own column when there is room for the workspace beside it, else folded
    // under the workspace (the groups move, keeping their state).
    private const double FoldBelow = 900;

    private void OnContentSizeChanged(object sender, SizeChangedEventArgs e)
    {
        bool folded = e.NewSize.Width < FoldBelow;
        if (folded == _folded && _mains.Length > 0) return;
        _folded = folded;
        if (folded)
        {
            InspectorScroll.Content = null;
            InspectorStack.Margin = new Thickness(0, 8, 0, 0);
            FoldedInspector.Child = InspectorStack;
        }
        else
        {
            FoldedInspector.Child = null;
            InspectorStack.ClearValue(FrameworkElement.MarginProperty);
            InspectorScroll.Content = InspectorStack;
        }
        ArrangeInspector();
    }

    private void ArrangeInspector()
    {
        bool has = _sides.Length > _page && _sides[_page] != null;
        InspectorPane.Visibility = has && !_folded ? Visibility.Visible : Visibility.Collapsed;
        FoldedInspector.Visibility = has && _folded ? Visibility.Visible : Visibility.Collapsed;
    }

    // Every page uses the viewport width, including pages with small intrinsic content.
    private void OnWorkspaceSizeChanged(object sender, SizeChangedEventArgs e) =>
        CenterStack.Width = Math.Max(0, e.NewSize.Width - CenterStack.Margin.Left - CenterStack.Margin.Right);

    // The previews keep a natural proportion as the workspace widens.
    private void OnCenterSizeChanged(object sender, SizeChangedEventArgs e)
    {
        double w = e.NewSize.Width;
        HeroFrame.Height = Math.Clamp(Math.Round(w * 0.52), 280, 580);
        NotifyStage.Height = Math.Clamp(Math.Round(w * 0.42), 260, 440);
    }

    private void OnLoupeToggled(object sender, RoutedEventArgs e)
    {
        Loupe.Visibility = _page == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        if (Loupe.Visibility == Visibility.Collapsed)
        {
            _loupeBrush.ImageSource = null;
            _loupeBmp = null;
        }
        UpdateHero();
    }

    // Fade and rise a few pixels; the transitions are attached one tick later so the start values stick.
    private void AnimateIn(UIElement e)
    {
        e.OpacityTransition = null;
        e.TranslationTransition = null;
        e.Opacity = 0;
        e.Translation = new Vector3(0, 10, 0);
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(240) };
            e.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(300) };
            e.Opacity = 1;
            e.Translation = Vector3.Zero;
        });
    }

    // The sidebar is a virtualizing list; its entries are set directly so they
    // follow the language even before the first list container is realized.
    private void LocalizeNav()
    {
        foreach (object? entry in Nav.Items)
        {
            if (entry is not ListViewItem item) continue;
            if (item.Content is NavLabel label) label.Text = Loc.T(label.Text);
            string name = AutomationProperties.GetName(item);
            if (!string.IsNullOrEmpty(name)) AutomationProperties.SetName(item, Loc.T(name));
        }
    }

    // A template's elements are created after the window: localize each one as it loads.
    private void OnLocalizeTemplate(object sender, RoutedEventArgs e)
    {
        if (sender is DependencyObject root) Loc.Localize(root);
    }
}
