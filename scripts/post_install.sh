#!/bin/sh
# Run by 'ninja install' after all files are placed.
# Installs the default config on first install; preserves it on upgrades.

config=/etc/rusrp/rusrp.toml
example=/etc/rusrp/rusrp.toml.example

if [ ! -f "$config" ]; then
    cp "$example" "$config"
    echo "rusrp: installed default config at $config"
    echo "rusrp: edit $config (set remote_host at minimum) before starting the service"
else
    echo "rusrp: existing config preserved at $config"
fi
