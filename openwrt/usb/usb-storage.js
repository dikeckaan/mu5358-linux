'use strict';
'require baseclass';
'require rpc';
'require ui';

/* The "USB flash drive" card on System > Device (mu5358/usb-storage, loaded by device.js through customize.sh):
 * the device as a removable disk for the computer, beside the USB network link. Talks to the rpcd object mu5358usb
 * (openwrt/usb/rpcd-mu5358usb), which runs mu5358-usb storage on|off|size N. */
var callGet = rpc.declare({ object: 'mu5358usb', method: 'get', expect: { '': {} }, nobatch: true });
var callSet = rpc.declare({ object: 'mu5358usb', method: 'set', params: [ 'kind', 'value' ], expect: { '': {} }, nobatch: true });

return baseclass.extend({
	render: function(root, before) {
		var card = E('section', { 'class': 'mud-card', 'id': 'mud-usb-disk-card' }, [
			E('h3', {}, _('USB flash drive')),
			E('div', { 'class': 'mud-r' }, [ E('span', { 'class': 'mud-k' }, _('State')), E('span', { 'class': 'mud-v', 'id': 'mud-usb-disk-state' }, '--') ]),
			E('div', { 'class': 'mud-r' }, [ E('span', { 'class': 'mud-k' }, _('Size')), E('span', { 'class': 'mud-v', 'id': 'mud-usb-disk-size' }, '--') ]),
			E('div', { 'class': 'mud-r' }, [ E('span', { 'class': 'mud-k' }, _('Exported to the computer')), E('span', { 'class': 'mud-v', 'id': 'mud-usb-disk-exported' }, '--') ]),
			E('div', { 'class': 'mud-device-field' }, [
				E('label', { 'for': 'mud-usb-disk-mib' }, _('Size of a new disk (MiB)')),
				E('input', { 'type': 'number', 'id': 'mud-usb-disk-mib', 'min': '64', 'step': '1', 'style': 'max-width:12em' })
			]),
			E('div', { 'class': 'mud-device-actions' }, [
				E('button', { 'class': 'mud-btn', 'id': 'mud-usb-disk-on' }, _('Turn on')),
				E('button', { 'class': 'mud-btn', 'id': 'mud-usb-disk-off' }, _('Turn off')),
				E('button', { 'class': 'mud-btn', 'id': 'mud-usb-disk-resize' }, _('Resize (while off)'))
			]),
			E('div', { 'class': 'mud-note' }, _('While the drive is on, the computer sees this device as a removable disk as well as the network link; turning it on or off replugs the USB link for a moment. The device itself does not touch the disk while it is exported.')),
			E('div', { 'class': 'mud-note', 'id': 'mud-usb-disk-msg', 'role': 'status', 'aria-live': 'polite' })
		]);
		root.insertBefore(card, before || null);
		this.card = card;
		this.wire();
		this.refresh();
		return card;
	},
	q: function(id) { return this.card.querySelector('#mud-usb-disk-' + id); },
	paint: function(s) {
		var on = s.storage === 'on';
		this.q('state').textContent = s.ok ? (on ? _('On') : _('Off')) : _('Unavailable');
		this.q('size').textContent = s.size_mib ? s.size_mib + ' MiB' : '--';
		this.q('exported').textContent = s.ok ? (s.exported ? _('Yes') : _('No')) : '--';
		if (!this.q('mib').value && s.size_mib) this.q('mib').value = s.size_mib;
		this.q('mib').max = s.free_mib ? Math.max(64, s.free_mib - 1024) : '';
		this.q('on').disabled = !s.ok || on;
		this.q('off').disabled = !s.ok || !on;
		this.q('resize').disabled = !s.ok || on;
	},
	refresh: function() {
		var self = this;
		return L.resolveDefault(callGet(), {}).then(function(s) { self.paint(s || {}); });
	},
	apply: function(kind, value) {
		var self = this, msg = this.q('msg');
		[ 'on', 'off', 'resize' ].forEach(function(id) { self.q(id).disabled = true; });
		msg.textContent = _('Working…');
		return L.resolveDefault(callSet(kind, value), {}).then(function(r) {
			if (!r || !r.ok) {
				msg.textContent = '';
				ui.addNotification(null, E('p', {}, (r && r.error) || _('The USB adapter gave no valid reply')), 'danger');
			} else {
				msg.textContent = r.message || '';
			}
			window.setTimeout(function() { self.refresh(); }, 3000);
			return self.refresh();
		});
	},
	wire: function() {
		var self = this;
		this.q('on').addEventListener('click', function() { self.apply('storage', 'on'); });
		this.q('off').addEventListener('click', function() { self.apply('storage', 'off'); });
		this.q('resize').addEventListener('click', function() {
			var mib = parseInt(self.q('mib').value, 10);
			if (!(mib >= 64)) { ui.addNotification(null, E('p', {}, _('The size must be at least 64 MiB')), 'warning'); return; }
			self.apply('storage_size', String(mib));
		});
	}
});
