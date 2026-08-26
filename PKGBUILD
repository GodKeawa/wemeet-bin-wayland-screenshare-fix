# Maintainer: sukanka<su975853527[AT]gmail.com>
# Contributor: Sam L. Yes <samlukeyes123 at gmail dot com>

_pkgname=wemeet
pkgname=$_pkgname-bin
provides=('wemeet' 'tencent-meeting')
pkgver=3.26.10.401
_pkgver_arm=3.26.10.401
_x86_md5=72e0e0023e1d1e6d4123fba28821aea1
_arm_md5=c06d6bc4a3370dbfb2f43bbc6ff8969e
pkgrel=5
pkgdesc="Tencent Video Conferencing, tencent meeting 腾讯会议"
arch=('x86_64' 'aarch64')
license=('unknown')
url="https://source.meeting.qq.com/download-center.html"

# Options for screenshare hook mode:
# - "none"     : Only hook handshake/negotiation. Bypasses all memory copy/color hooks.
# - "straight" : Hook and copy frame without swapping colors (prevents crashes, keeps correct colors on modern systems).
# - "swap"     : Hook, copy, and swap BGRx/RGBx color channels (original author's behavior).
_hook_mode="none"

# Options for debug mode:
# - 0 : Disable debug logs.
# - 1 : Enable detailed debug logs (printed to stdout/stderr).
_debug_mode=1

source_x86_64=("${_pkgname}-${pkgver}-x86_64.deb::https://updatecdn.meeting.qq.com/cos/${_x86_md5}/TencentMeeting_0300000000_${pkgver}_x86_64_default.publish.deb"
)
source_aarch64=("${_pkgname}-${_pkgver_arm}-aarch64.deb::https://updatecdn.meeting.qq.com/cos/${_arm_md5}/TencentMeeting_0300000000_${_pkgver_arm}_arm64_default.publish.deb")
source=("${_pkgname}".sh 'wrap.c' 'libhook.c' 'patch.py')
depends=(
    # most deps are not used, but kept for a
    bash
    qt5-x11extras libxinerama
    libpulse # 无 pulseaudio 无法连接到系统音频
    # dependencies detected by namcap
    gcc-libs qt5-declarative libglvnd libxfixes alsa-lib openssl
    libxrandr libxext libx11 hicolor-icon-theme glibc zlib libxcomposite
    qt5-base systemd-libs libxdamage qt5-svg
    libyuv
)
optdepends=(
    'qt5-wayland: Wayland support'
    'bubblewrap: Fix abnormal text color in dark mode and prevent messing files.'
)
makedepends=('patchelf')
sha512sums=('e8290375930e02dde232f48c03f76a3473cd70e5beead059709d8b5201a3141ef8e6399a1867859a1ae4e8cbaab4e350a3922c994ac1ddc2094f54095b80178a'
            'f98e9ae5842c05a19ad4f883c8f9d88ef3b64e04b034e7fd8b23ddca81510f0bd38688ad7c63ddf8badaa727a7b599ceede87419e9694c06d7a4b06138b94c15'
            '8858f6fba8679c3d6f8503fe60af6b9de48f91338fe2335ca867ac15c445a4794b7a5a6f9ebe76bef31cd7f1483a8fe56ca07ef0215c44349652e5846cdfa31e'
            'b59f2b4890ad2b573902cdf90a55cdee27ecb786e29b355ba984af0e82b715f8445bd8df19b0ca7b1a85be7367c357c8799f74d16fe5c6ce6e60965b200e1ef6')
sha512sums_x86_64=('acaa1eba8eccd3a5bd4cb57dc0fadce5c33950857677783944ac2bfbefbed927ca3b4b7d9d0c9e864dcdc3b9f9f8a359c456e9b63b38ac0fa9436fc336aa9ea7')
sha512sums_aarch64=('6fb54c4972b6ebaf8e1ce576b4ea075ec1b1cd5d02702feef5382712a8bafc2ff85cbd6ab43c90e6e6c55627da77ecc1c8c58f4154e0a160247387f5b2972abc')

# strip了反而变大
options=(!strip)

prepare() {
    cd "$srcdir"
    tar xpf data.tar.xz

    pushd usr/share/applications
    sed -i 's|^Exec=.*|Exec=wemeet %u|g;s|^Icon=.*|Icon=wemeet|g' ${_pkgname}app.desktop
    sed -i '$i Comment=Tencent Meeting Linux Client\nComment[zh_CN]=腾讯会议Linux客户端\nKeywords=wemeet;tencent;meeting;' \
        "$srcdir/usr/share/applications/wemeetapp.desktop"
    popd

    pushd opt/$_pkgname
    if [ -d 'icons' ]; then
        for res in 16 32 64 128 256; do
            install -dm755 "$srcdir/usr/share/icons/hicolor/${res}x${res}/apps"
            mv "icons/hicolor/${res}x${res}/mimetypes/${_pkgname}app.png" \
                "$srcdir/usr/share/icons/hicolor/${res}x${res}/apps/${_pkgname}app.png"
        done
    else
        echo 'icons directory not found'
    fi

    # rm bin/qt.conf
    sed -i "s|^Prefix.*|Prefix = /usr/lib/wemeet|" bin/qt.conf
    patchelf --set-rpath '$ORIGIN:/usr/lib/wemeet' bin/wemeetapp
    popd

    pushd opt/$_pkgname/bin

    find modules/ -type f -name '*.so' | xargs -I {} patchelf --set-rpath '$ORIGIN:/usr/lib/wemeet' {}
    popd

    # Apply screenshare patch
    python3 "$srcdir/patch.py" "$srcdir/opt/$_pkgname"
}

build() {
    cd "$srcdir"
    read -ra openssl_args < <(pkgconf --libs openssl)
    read -ra libpulse_args < <(pkgconf --cflags --libs libpulse)
    # Comment out `-D WRAP_FORCE_SINK_HARDWARE` to disable the patch that forces wemeet detects sink as hardware sink
    "${CC:-cc}" $CFLAGS -Wall -Wextra -fPIC -shared "${openssl_args[@]}" "${libpulse_args[@]}" -o libwemeetwrap.so wrap.c -D WRAP_FORCE_SINK_HARDWARE

    # Compile libhook.so
    local cflags="-O3 -shared -fPIC -ldl"
    if [ "$_hook_mode" = "swap" ]; then
        cflags="$cflags -DWEMEET_HOOK_MODE_SWAP"
    fi
    if [ "$_debug_mode" = "1" ]; then
        cflags="$cflags -DWEMEET_DEBUG"
    fi
    read -ra pipewire_args < <(pkgconf --cflags libpipewire-0.3)
    "${CC:-cc}" $cflags -o libhook.so libhook.c "${pipewire_args[@]}"
}

package() {
    cd "$srcdir"
    cp -r usr "$pkgdir"
    cd opt/$_pkgname

    install -Dm755 "$srcdir/$_pkgname.sh" "$pkgdir/usr/bin/$_pkgname"
    ln -s "/usr/bin/$_pkgname" "$pkgdir/usr/bin/$_pkgname-x11"
    install -Dm644 $_pkgname.svg -t "$pkgdir/usr/share/icons/hicolor/scalable/apps"

    # libbugly is not likely to be necessary
    install -Dm755 lib/lib{desktop_common,crash_guard,ImSDK,nxui*,qt_*,ui*,wemeet*,xcast*,xnn*}.so \
        -t "$pkgdir/usr/lib/$_pkgname"
    if [ -f 'lib/libcrbase.so' ]; then
        install -Dm755 lib/libcrbase.so -t "$pkgdir/usr/lib/$_pkgname"
    else
        echo 'lib/libcrbase.so not found'
    fi
    # copy Qt
    cp -r plugins resources translations "$pkgdir/usr/lib/$_pkgname"
    cp -a lib/lib{Qt,icu}* "$pkgdir/usr/lib/$_pkgname"

    find "$pkgdir/usr/lib/$_pkgname" -type f -name '*.so*' | xargs -I {} patchelf --set-rpath '$ORIGIN:/usr/lib/wemeet' {}

    install -dm755 "$pkgdir/opt/$_pkgname"
    cp -r bin "$pkgdir/opt/$_pkgname"
    ln -s raw/xcast.conf "$pkgdir/opt/$_pkgname/bin/xcast.conf"
    install -Dm755 "$srcdir/libwemeetwrap.so" -t "$pkgdir/usr/lib/$_pkgname"
    install -Dm755 "$srcdir/libhook.so" -t "$pkgdir/usr/lib/$_pkgname"
}
