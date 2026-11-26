using System.Numerics;
using System.Runtime.CompilerServices;
using Microsoft.Graphics.Canvas.Geometry;
using Microsoft.UI;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace Atrium.Settings;

// Visual states require normal dependency properties: WinUI cannot animate
// custom attached properties. Keep these logical paints separate from the
// renderer brush so leaving a state restores the original binding correctly.
public sealed partial class ContinuousSurface : Grid
{
    public static readonly DependencyProperty PaintProperty = DependencyProperty.Register(
        nameof(Paint), typeof(Brush), typeof(ContinuousSurface), new PropertyMetadata(null,
            (owner, args) => ContinuousCorners.SetPaint(owner, (Brush?)args.NewValue)));
    public Brush? Paint { get => (Brush?)GetValue(PaintProperty); set => SetValue(PaintProperty, value); }
    public static readonly DependencyProperty EdgeProperty = DependencyProperty.Register(
        nameof(Edge), typeof(Brush), typeof(ContinuousSurface), new PropertyMetadata(null,
            (owner, args) => ContinuousCorners.SetEdge(owner, (Brush?)args.NewValue)));
    public Brush? Edge { get => (Brush?)GetValue(EdgeProperty); set => SetValue(EdgeProperty, value); }
    public ContinuousSurface()
    {
        ContinuousCorners.SetEnabled(this, true);
    }
}

// Applies the analytic boundary as an alpha mask. Native CornerRadius / cubic
// clips are removed; layout, content, focus, input and motion stay with WinUI.
public sealed partial class ContinuousCorners : DependencyObject
{
    public static readonly DependencyProperty EnabledProperty = DependencyProperty.RegisterAttached(
        "Enabled", typeof(bool), typeof(ContinuousCorners), new PropertyMetadata(false, EnabledChanged));
    public static bool GetEnabled(DependencyObject value) => (bool)value.GetValue(EnabledProperty);
    public static void SetEnabled(DependencyObject value, bool enabled) => value.SetValue(EnabledProperty, enabled);
    public static readonly DependencyProperty PaintProperty = DependencyProperty.RegisterAttached(
        "Paint", typeof(Brush), typeof(ContinuousCorners), new PropertyMetadata(null, PaintChanged));
    public static Brush? GetPaint(DependencyObject value) => (Brush?)value.GetValue(PaintProperty);
    public static void SetPaint(DependencyObject value, Brush? brush) => value.SetValue(PaintProperty, brush);
    public static readonly DependencyProperty EdgeProperty = DependencyProperty.RegisterAttached(
        "Edge", typeof(Brush), typeof(ContinuousCorners), new PropertyMetadata(null, PaintChanged));
    public static Brush? GetEdge(DependencyObject value) => (Brush?)value.GetValue(EdgeProperty);
    public static void SetEdge(DependencyObject value, Brush? brush) => value.SetValue(EdgeProperty, brush);
    public static readonly DependencyProperty ClipChildrenProperty = DependencyProperty.RegisterAttached(
        "ClipChildren", typeof(bool), typeof(ContinuousCorners), new PropertyMetadata(false));
    public static bool GetClipChildren(DependencyObject value) => (bool)value.GetValue(ClipChildrenProperty);
    public static void SetClipChildren(DependencyObject value, bool clip) => value.SetValue(ClipChildrenProperty, clip);
    private static readonly ConditionalWeakTable<FrameworkElement, State> States = [];
    private static readonly ConditionalWeakTable<Control, FocusChrome> FocusOwners = [];
    private static readonly SolidColorBrush Transparent = new(Colors.Transparent);
    private static void PaintChanged(DependencyObject value, DependencyPropertyChangedEventArgs args)
    {
        if (value is FrameworkElement element && States.TryGetValue(element, out var state)) state.RefreshPaint();
    }

    private static void EnabledChanged(DependencyObject value, DependencyPropertyChangedEventArgs args)
    {
        if (value is not FrameworkElement element || args.NewValue is not true) return;
        element.Loaded += (_, _) => Install(element);
        if (element.IsLoaded) Install(element);
    }

    internal static void Install(FrameworkElement root)
    {
        if (!root.IsLoaded)
        {
            if (!GetEnabled(root)) SetEnabled(root, true);
            return;
        }
        Visit(root);
    }

    private static void Visit(DependencyObject node)
    {
        if (node is FrameworkElement element && !States.TryGetValue(element, out _) && State.Radius(element) > 0)
            States.Add(element, new State(element));
        int count = VisualTreeHelper.GetChildrenCount(node);
        for (int i = 0; i < count; ++i) Visit(VisualTreeHelper.GetChild(node, i));
    }

    internal static Brush? Original(FrameworkElement element) => States.TryGetValue(element, out var state)
        ? state.Background : State.GetBackground(element);
    internal static double RadiusOf(FrameworkElement element) => States.TryGetValue(element, out var state) ? state.Corner : State.Radius(element);
    // Acquiring a handout visual conflicts with XAML Translation/Scale and their
    // transitions. These brushes only need the compositor; the connected root
    // supplies the same one without taking ownership of an animated element.
    private static Compositor CompositorFor(FrameworkElement element) =>
        ElementCompositionPreview.GetElementVisual(element.XamlRoot?.Content ?? element).Compositor;

    private sealed class State
    {
        private readonly FrameworkElement _element;
        private readonly DependencyProperty _radiusProperty, _backgroundProperty, _strokeProperty;
        private AnalyticBrush? _fill;
        private AnalyticBrush? _outline;
        private SpriteVisual? _outlineVisual;
        private bool _changing;
        internal double Corner;
        internal Brush? Background;
        private Brush? _stroke;
        private readonly Brush? _baseBackground, _baseStroke;
        private readonly FocusChrome? _focus;
        private CompositionGeometricClip? _nativeClip;
        private (double Width, double Height, double Radius) _clipGeometry;
        internal void RefreshPaint()
        {
            Background = GetPaint(_element) ?? _baseBackground;
            _stroke = GetEdge(_element) ?? _baseStroke;
            Update();
        }

        internal State(FrameworkElement element)
        {
            _element = element;
            (_radiusProperty, _backgroundProperty, _strokeProperty) = element switch
            {
                Border => (Border.CornerRadiusProperty, Border.BackgroundProperty, Border.BorderBrushProperty),
                Grid => (Grid.CornerRadiusProperty, Panel.BackgroundProperty, Grid.BorderBrushProperty),
                ContentPresenter => (ContentPresenter.CornerRadiusProperty, ContentPresenter.BackgroundProperty, ContentPresenter.BorderBrushProperty),
                Rectangle => (Rectangle.RadiusXProperty, Shape.FillProperty, Shape.StrokeProperty),
                _ => throw new InvalidOperationException(),
            };
            Corner = Radius(element);
            _baseBackground = (Brush?)element.GetValue(_backgroundProperty);
            _baseStroke = (Brush?)element.GetValue(_strokeProperty);
            Background = GetPaint(element) ?? _baseBackground;
            _stroke = GetEdge(element) ?? _baseStroke;
            element.RegisterPropertyChangedCallback(_backgroundProperty, (_, _) =>
            {
                if (_changing) return;
                Background = (Brush?)element.GetValue(_backgroundProperty);
                Update();
            });
            element.RegisterPropertyChangedCallback(_strokeProperty, (_, _) =>
            {
                if (_changing) return;
                _stroke = (Brush?)element.GetValue(_strokeProperty);
                Update();
            });
            element.RegisterPropertyChangedCallback(_radiusProperty, (_, _) =>
            {
                if (_changing) return;
                Corner = Radius(element);
                Update();
            });
            var thicknessProperty = element switch
            {
                Border => Border.BorderThicknessProperty,
                Grid => Grid.BorderThicknessProperty,
                ContentPresenter => ContentPresenter.BorderThicknessProperty,
                _ => Shape.StrokeThicknessProperty,
            };
            element.RegisterPropertyChangedCallback(thicknessProperty, (_, _) => { if (!_changing) Update(); });
            element.SizeChanged += (_, _) => Update();
            element.ActualThemeChanged += (_, _) => Update();
            element.Unloaded += (_, _) => { _fill?.Deactivate(); _outline?.Deactivate(); };
            element.Loaded += (_, _) => Update();
            for (var parent = Control.GetIsTemplateFocusTarget(element) ? VisualTreeHelper.GetParent(element) : null;
                 parent != null; parent = VisualTreeHelper.GetParent(parent))
            {
                if (parent is not Control control) continue;
                if (control is not ScrollViewer and not UserControl and not ItemsControl && (control.IsTabStop || control.UseSystemFocusVisuals))
                    _focus = FocusOwners.GetValue(control, c => new FocusChrome(c, element));
                break;
            }
            Update();
        }

        internal static double Radius(FrameworkElement e) => e switch
        {
            Border b => b.CornerRadius.TopLeft,
            Grid p => p.CornerRadius.TopLeft,
            ContentPresenter c => c.CornerRadius.TopLeft,
            Rectangle r => r.RadiusX,
            _ => 0,
        };
        internal static Brush? GetBackground(FrameworkElement e) => e switch
        {
            Border b => b.Background,
            Panel p => p.Background,
            Control c => c.Background,
            ContentPresenter c => c.Background,
            Rectangle r => r.Fill,
            _ => null,
        };
        private Thickness Thickness => _element switch
        {
            Border b => b.BorderThickness,
            Grid p => p.BorderThickness,
            ContentPresenter c => c.BorderThickness,
            Rectangle r => new Thickness(r.StrokeThickness),
            _ => new Thickness(),
        };
        private TimeSpan Transition => _element switch
        {
            Border b => b.BackgroundTransition?.Duration ?? TimeSpan.Zero,
            Panel p => p.BackgroundTransition?.Duration ?? TimeSpan.Zero,
            ContentPresenter c => c.BackgroundTransition?.Duration ?? TimeSpan.Zero,
            _ => TimeSpan.Zero,
        };

        private void Update()
        {
            if (_element.ActualWidth <= 0 || _element.ActualHeight <= 0) return;
            _changing = true;
            try
            {
                _element.SetValue(_radiusProperty, _element is Rectangle ? 0d : new CornerRadius(0));
                if (_element is Rectangle rect) rect.RadiusY = 0;
                // Keep live acrylic and bitmap brushes connected to XAML: image
                // invalidation, alignment, transitions and material policies stay
                // native. The finite path clip below is still the A08 gap;
                // it must not be described as analytic pixel coverage.
                if (Background is AcrylicBrush or ImageBrush)
                {
                    ApplyNativeClip();
                    _element.SetValue(_backgroundProperty, Background);
                }
                else
                {
                    if (GetClipChildren(_element)) ApplyNativeClip();
                    else if (_nativeClip != null) ElementCompositionPreview.GetElementVisual(_element).Clip = null;
                    if (Background != null)
                    {
                        _fill ??= new AnalyticBrush(_element);
                        _fill.Update(Corner, 0, Background, Transition);
                    }
                    _element.SetValue(_backgroundProperty, Background == null ? null : _fill);
                }
                _element.SetValue(_strokeProperty, Transparent);
                double width = Math.Max(Math.Max(Thickness.Left, Thickness.Right), Math.Max(Thickness.Top, Thickness.Bottom));
                if (_stroke != null && width > 0)
                {
                    if (_outline == null)
                    {
                        _outline = new AnalyticBrush(_element, false);
                        _outlineVisual = CompositorFor(_element).CreateSpriteVisual();
                        _outlineVisual.Brush = _outline.Native;
                        ElementCompositionPreview.SetElementChildVisual(_element, _outlineVisual);
                    }
                    _outline.Update(Corner, width, _stroke, Transition);
                    _outlineVisual!.Size = new Vector2((float)_element.ActualWidth, (float)_element.ActualHeight);
                    _outlineVisual.Opacity = 1;
                }
                else
                {
                    _outlineVisual?.Opacity = 0;
                }

                _focus?.Update(Corner);
            }
            finally { _changing = false; }
        }

        private void ApplyNativeClip()
        {
            var next = (_element.ActualWidth, _element.ActualHeight, Corner);
            if (_nativeClip != null && _clipGeometry == next)
            {
                ElementCompositionPreview.GetElementVisual(_element).Clip = _nativeClip;
                return;
            }
            _clipGeometry = next;
            float w = (float)next.ActualWidth, h = (float)next.ActualHeight;
            var p = CornerProfile.For(w, h, (float)Corner);
            // No CanvasDevice: this path is device-independent. The points are a
            // subpixel tessellation of F=0 for the platform clip. Its finite
            // line segments are NOT a G3 visible boundary (audit A08).
            using var builder = new CanvasPathBuilder(null);
            int steps = Math.Max(16, (int)Math.Ceiling(Math.PI * Math.Sqrt(p.Reach / 0.01) / 2));
            for (int corner = 0; corner < 4; ++corner)
            {
                for (int i = 0; i <= steps; ++i)
                {
                    var q = p.Point(i * Math.PI / (2 * steps));
                    Vector2 point = corner switch
                    {
                        0 => new(w - p.Reach + p.Reach * q.X, p.Reach + p.Reach * q.Y),
                        1 => new(w - p.Reach - p.Reach * q.Y, h - p.Reach + p.Reach * q.X),
                        2 => new(p.Reach - p.Reach * q.X, h - p.Reach - p.Reach * q.Y),
                        _ => new(p.Reach + p.Reach * q.Y, p.Reach - p.Reach * q.X),
                    };
                    if (corner == 0 && i == 0) builder.BeginFigure(point); else builder.AddLine(point);
                }
            }

            builder.EndFigure(CanvasFigureLoop.Closed);
            using var geometry = CanvasGeometry.CreatePath(builder);
            var compositor = ElementCompositionPreview.GetElementVisual(_element).Compositor;
            var path = compositor.CreatePathGeometry(new CompositionPath(geometry));
            _nativeClip ??= compositor.CreateGeometricClip();
            _nativeClip.Geometry = path;
            ElementCompositionPreview.GetElementVisual(_element).Clip = _nativeClip;
        }
    }

    private sealed class FocusChrome
    {
        private readonly Control _control;
        private readonly FrameworkElement _owner;
        private readonly AnalyticBrush _primary, _secondary;
        private readonly SpriteVisual _outer, _inner;
        private readonly ContainerVisual _visual;
        private readonly double _primaryWidth, _secondaryWidth;
        internal FocusChrome(Control control, FrameworkElement owner)
        {
            _control = control;
            _owner = owner;
            _primaryWidth = control.FocusVisualPrimaryThickness.Left;
            _secondaryWidth = control.FocusVisualSecondaryThickness.Left;
            // Keep the original brushes (including their theme bindings), widths
            // and margin. Suppress only the platform's circular focus geometry.
            control.UseSystemFocusVisuals = true;
            control.FocusVisualPrimaryThickness = new Thickness(0);
            control.FocusVisualSecondaryThickness = new Thickness(0);
            var compositor = CompositorFor(owner);
            _primary = new AnalyticBrush(owner, false);
            _secondary = new AnalyticBrush(owner, false);
            _outer = compositor.CreateSpriteVisual(); _outer.Brush = _primary.Native;
            _inner = compositor.CreateSpriteVisual(); _inner.Brush = _secondary.Native;
            _visual = compositor.CreateContainerVisual();
            _visual.Children.InsertAtTop(_outer);
            _visual.Children.InsertAtTop(_inner);
            ElementCompositionPreview.SetElementChildVisual(control, _visual);
            control.RegisterPropertyChangedCallback(UIElement.FocusStateProperty, (_, _) => Show());
            Show();
        }
        internal void Update(double radius)
        {
            var margin = _control.FocusVisualMargin;
            double w = Math.Max(0, _owner.ActualWidth - margin.Left - margin.Right);
            double h = Math.Max(0, _owner.ActualHeight - margin.Top - margin.Bottom);
            radius -= margin.Left;
            _primary.Update(radius, _primaryWidth + _secondaryWidth, _control.FocusVisualPrimaryBrush, TimeSpan.Zero, w, h);
            _outer.Size = new Vector2((float)w, (float)h);
            _secondary.Update(Math.Max(0, radius - _primaryWidth), _secondaryWidth, _control.FocusVisualSecondaryBrush, TimeSpan.Zero,
                Math.Max(0, w - 2 * _primaryWidth), Math.Max(0, h - 2 * _primaryWidth));
            _inner.Size = new Vector2((float)Math.Max(0, w - 2 * _primaryWidth), (float)Math.Max(0, h - 2 * _primaryWidth));
            _inner.Offset = new Vector3((float)_primaryWidth, (float)_primaryWidth, 0);
            _inner.Opacity = _secondaryWidth > 0 ? 1 : 0;
            _outer.Opacity = _primaryWidth > 0 ? 1 : 0;
            var position = _owner.TransformToVisual(_control).TransformPoint(default);
            _visual.Offset = new Vector3((float)(position.X + margin.Left), (float)(position.Y + margin.Top), 0);
        }
        private void Show() => _visual.Opacity = _control.FocusState == FocusState.Keyboard ? 1 : 0;
    }

    private sealed partial class AnalyticBrush : XamlCompositionBrushBase
    {
        private readonly FrameworkElement _owner;
        private readonly Compositor _compositor;
        private readonly CompositionMaskBrush _mask;
        private readonly CompositionColorBrush _color;
        private readonly CompositionNineGridBrush _nine;
        private readonly CompositionSurfaceBrush _maskSource;
        private readonly SpriteVisual? _raster;
        private readonly CompositionVisualSurface? _rasterSurface;
        private Brush? _source;
        private long _sourceToken;
        private long _gradientStartToken, _gradientEndToken;
        private CompositionLinearGradientBrush? _gradient;
        private MaskKey? _activeMask;
        private (double W, double H, double R, double Stroke, double Scale) _geometry;
        private int _generation;
        internal CompositionBrush Native => _mask;

        internal AnalyticBrush(FrameworkElement owner, bool xaml = true)
        {
            _owner = owner;
            _compositor = CompositorFor(owner);
            _color = _compositor.CreateColorBrush(Colors.Transparent);
            _mask = _compositor.CreateMaskBrush();
            _mask.Source = _color;
            _maskSource = _compositor.CreateSurfaceBrush();
            _maskSource.Stretch = CompositionStretch.Fill;
            _nine = _compositor.CreateNineGridBrush();
            _nine.Source = _maskSource;
            _mask.Mask = _nine;
            if (xaml)
            {
                // XAML adds its own primitive mask. A surface is a supported
                // input there; nesting a CompositionMaskBrush is not.
                _raster = _compositor.CreateSpriteVisual();
                _raster.Brush = _mask;
                _rasterSurface = _compositor.CreateVisualSurface();
                _rasterSurface.SourceVisual = _raster;
                var surfaceBrush = _compositor.CreateSurfaceBrush(_rasterSurface);
                surfaceBrush.Stretch = CompositionStretch.Fill;
                CompositionBrush = surfaceBrush;
            }
        }

        internal void Update(double radius, double stroke, Brush? next, TimeSpan transition, double? width = null, double? height = null)
        {
            var nextGeometry = (width ?? _owner.ActualWidth, height ?? _owner.ActualHeight, radius, stroke, _owner.XamlRoot?.RasterizationScale ?? 1);
            if (_geometry != nextGeometry)
            {
                _geometry = nextGeometry;
                if (_raster != null)
                {
                    _raster.Size = new Vector2((float)(_geometry.W * _geometry.Scale), (float)(_geometry.H * _geometry.Scale));
                    _rasterSurface!.SourceSize = _raster.Size;
                }
                _ = SetMaskAsync(++_generation);
            }
            if (!ReferenceEquals(_source, next))
            {
                Deactivate();
                _source = next;
                if (_source is SolidColorBrush)
                    _sourceToken = _source.RegisterPropertyChangedCallback(SolidColorBrush.ColorProperty, (_, _) => SetSource(TimeSpan.Zero));
                if (_source is LinearGradientBrush)
                {
                    _gradientStartToken = _source.RegisterPropertyChangedCallback(LinearGradientBrush.StartPointProperty, (_, _) => SetGradientPosition());
                    _gradientEndToken = _source.RegisterPropertyChangedCallback(LinearGradientBrush.EndPointProperty, (_, _) => SetGradientPosition());
                }
                SetSource(transition);
            }
        }

        internal void Deactivate()
        {
            if (_source is SolidColorBrush) _source.UnregisterPropertyChangedCallback(SolidColorBrush.ColorProperty, _sourceToken);
            if (_source is LinearGradientBrush)
            {
                _source.UnregisterPropertyChangedCallback(LinearGradientBrush.StartPointProperty, _gradientStartToken);
                _source.UnregisterPropertyChangedCallback(LinearGradientBrush.EndPointProperty, _gradientEndToken);
            }
            _source = null;
        }

        private void SetSource(TimeSpan transition)
        {
            if (_source is LinearGradientBrush linear)
            {
                _gradient ??= _compositor.CreateLinearGradientBrush();
                _gradient.ColorStops.Clear();
                foreach (var stop in linear.GradientStops) _gradient.ColorStops.Add(_compositor.CreateColorGradientStop((float)stop.Offset, stop.Color));
                SetGradientPosition();
                _mask.Source = _gradient;
                return;
            }
            _mask.Source = _color;
            var target = _source is SolidColorBrush solid ? solid.Color : Colors.Transparent;
            if (transition > TimeSpan.Zero)
            {
                var animation = _compositor.CreateColorKeyFrameAnimation();
                // Auto can sweep through purple between neutral and blue UI
                // states. Interpolate the channels without introducing a hue.
                animation.InterpolationColorSpace = CompositionColorSpace.Rgb;
                animation.Duration = transition;
                animation.InsertKeyFrame(1, target, _compositor.CreateLinearEasingFunction());
                _color.StartAnimation("Color", animation);
            }
            else { _color.StopAnimation("Color"); _color.Color = target; }
        }

        private void SetGradientPosition()
        {
            if (_source is not LinearGradientBrush linear || _gradient == null) return;
            _gradient.StartPoint = new Vector2((float)linear.StartPoint.X, (float)linear.StartPoint.Y);
            _gradient.EndPoint = new Vector2((float)linear.EndPoint.X, (float)linear.EndPoint.Y);
        }

        private async Task SetMaskAsync(int ticket)
        {
            var (width, height, radius, stroke, rawScale) = _geometry;
            float scale = (float)rawScale, w = (float)width * scale, h = (float)height * scale, r = (float)radius * scale, s = (float)stroke * scale;
            var profile = CornerProfile.For(w, h, r);
            var inner = CornerProfile.For(Math.Max(0, w - 2 * s), Math.Max(0, h - 2 * s), Math.Max(0, r - s));
            var key = new MaskKey(profile, inner, s);
            _nine.SetInsets(profile.Reach);
            _nine.SetInsetScales(_raster == null ? 1 / scale : 1);
            if (_activeMask == key && _maskSource.Surface != null) return;
            var surface = await MaskForAsync(key);
            if (ticket != _generation) return;
            _maskSource.Surface = surface;
            _activeMask = key;
        }
    }

    private readonly record struct MaskKey(CornerProfile Outer, CornerProfile Inner, float Stroke);
    private static readonly Dictionary<MaskKey, (long Bytes, Task<LoadedImageSurface> Surface)> Masks = [];
    private static long _maskBytes;
    private static Task<LoadedImageSurface> MaskForAsync(MaskKey key)
    {
        if (Masks.TryGetValue(key, out var cached)) return cached.Surface;
        int inset = (int)MathF.Ceiling(key.Outer.Reach) + 1, size = inset * 2 + 1;
        long bytes = (long)size * size * 4;
        while (Masks.Count > 0 && (Masks.Count >= 64 || _maskBytes + bytes > 4 * 1024 * 1024))
        {
            var first = Masks.First();
            _maskBytes -= first.Value.Bytes;
            Masks.Remove(first.Key);
        }
        byte[] pixels = new byte[size * size * 4];
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                float a = key.Outer.Coverage(x + 0.5f, y + 0.5f, size, size);
                if (key.Stroke > 0)
                {
                    a = Math.Max(0, a - key.Inner.Coverage(x + 0.5f - key.Stroke, y + 0.5f - key.Stroke,
                        size - 2 * key.Stroke, size - 2 * key.Stroke));
                }

                int offset = (y * size + x) * 4;
                pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = pixels[offset + 3] = (byte)MathF.Round(a * 255);
            }
        }

        var surface = LoadPixelsAsync((uint)size, (uint)size, pixels);
        Masks.Add(key, (bytes, surface));
        _maskBytes += bytes;
        return surface;
    }

    private static async Task<LoadedImageSurface> LoadPixelsAsync(uint width, uint height, byte[] pixels)
    {
        using var stream = new InMemoryRandomAccessStream();
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, width, height, 96, 96, pixels);
        await encoder.FlushAsync();
        stream.Seek(0);
        var surface = LoadedImageSurface.StartLoadFromStream(stream);
        var loaded = new TaskCompletionSource();
        surface.LoadCompleted += (_, _) => loaded.TrySetResult();
        await loaded.Task;
        return surface;
    }
}
