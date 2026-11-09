# nni login setup (shipped by nni-base)
case ":$PATH:" in *:/opt/bb/bin:*) ;; *) PATH="$PATH:/opt/bb/bin" ;; esac
export PATH
case $- in
  *i*)
    NNI_DISTRO=$(cat /etc/nni/distro 2>/dev/null || echo ynnios)
    [ -f "/usr/share/nni/fastfetch/$NNI_DISTRO.jsonc" ] &&
      alias fastfetch="fastfetch -c /usr/share/nni/fastfetch/$NNI_DISTRO.jsonc"
    export PAGER=${PAGER:-less}
    if [ -z "$NNI_WELCOMED" ] && [ -r /etc/os-release ]; then
      NNI_WELCOMED=1; export NNI_WELCOMED
      NNI_PRETTY=$(sed -n 's/^PRETTY_NAME="\(.*\)"/\1/p' /etc/os-release)
      echo "Welcome to ${NNI_PRETTY:-nni} - 'handbook' for help, 'nni' for settings, 'svc' for services."
    fi
    ;;
esac
