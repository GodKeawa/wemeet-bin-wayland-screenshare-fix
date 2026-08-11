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

source_x86_64=("${_pkgname}-${pkgver}-x86_64.deb::https://updatecdn.meeting.qq.com/cos/${_x86_md5}/TencentMeeting_0300000000_${pkgver}_x86_64_default.publish.deb"
)
source_aarch64=("${_pkgname}-${_pkgver_arm}-aarch64.deb::https://updatecdn.meeting.qq.com/cos/${_arm_md5}/TencentMeeting_0300000000_${_pkgver_arm}_arm64_default.publish.deb")
source=("${_pkgname}".sh 'wrap.c' 'hook.asm' 'hook.ld' 'patch.py')
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
makedepends=('patchelf' 'nasm' 'lld')
sha512sums=('f319307c47102deec52e8c3d3080415dcdb694f1189586d11afe200c1805c49a1da2e5969415ef4bce48e974bb63fa12af97db3a4789c5818e451f25bc656b83'
            'f98e9ae5842c05a19ad4f883c8f9d88ef3b64e04b034e7fd8b23ddca81510f0bd38688ad7c63ddf8badaa727a7b599ceede87419e9694c06d7a4b06138b94c15'
            '299d0231a714db1ed0286716cfbe3cb753e4c27c37bf64bc91428a6dbc7fb948c6d2365d3ba01fda3df541b608656e4b76d0cb8994b237ce6afb0f0ea8932195'
            '2788807f62b9a239b2c7ddc5e902957a22e868a1df4fd495765966b2ffabb1d315d96c384b4cc8aff76ea59f7dd8f82c344ecea7f047393414a12146acbad433'
            '8a60af69831470d37229f708ae273dad5ff62afa222d55ccd2775f735271f503c79397306dd5cfca426919ace4b09fd29c3dce79faefc16b861c352a29f44cc8')
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
    if [ "$_hook_mode" = "none" ]; then
        python3 "$srcdir/patch.py" --no-color-hook "$srcdir/opt/$_pkgname"
    else
        python3 "$srcdir/patch.py" "$srcdir/opt/$_pkgname"
    fi
}

build() {
    cd "$srcdir"
    read -ra openssl_args < <(pkgconf --libs openssl)
    read -ra libpulse_args < <(pkgconf --cflags --libs libpulse)
    # Comment out `-D WRAP_FORCE_SINK_HARDWARE` to disable the patch that forces wemeet detects sink as hardware sink
    "${CC:-cc}" $CFLAGS -Wall -Wextra -fPIC -shared "${openssl_args[@]}" "${libpulse_args[@]}" -o libwemeetwrap.so wrap.c -D WRAP_FORCE_SINK_HARDWARE

    # Compile libhook.so
    local nasm_flags=""
    if [ "$_hook_mode" = "swap" ]; then
        nasm_flags="-dSWAP_COLORS"
    fi
    nasm $nasm_flags -felf64 hook.asm -o hook.o
    ld.lld --gc-sections --build-id=none -z noseparate-code -z now -shared -e 0 -T hook.ld -L/usr/lib64 -lc hook.o -o libhook.so
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
