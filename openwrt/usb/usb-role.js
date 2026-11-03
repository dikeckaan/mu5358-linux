'use strict';
'require baseclass';
'require rpc';
'require ui';

/* The "USB role" card on System > Device (mu5358/usb-role, loaded by device.js through customize.sh): the USB-C port's
 * policy through the port controller (mu5358-usb auto|host|device, rpcd mu5358usb). Host mode undoes itself after
 * 60 s unless kept from a shell (touch /run/mu5358/usb-host-keep): the port is the way to the computer. */
var callGet = rpc.declare({ object: 'mu5358usb', method: 'get', expect: { '': {} }, nobatch: true });
var callSet = rpc.declare({ object: 'mu5358usb', method: 'set', params: [ 'kind', 'value' ], expect: { '': {} }, nobatch: true });

return baseclass.extend({
	render: function(root, before) {
		var card = E('section', { 'class': 'mud-card', 'id': 'mud-usb-role-card' }, [
			E('h3', {}, _('USB role')),
			E('div', { 'class': 'mud-r' }, [ E('span', { 'class': 'mud-k' }, _('Policy')), E('span', { 'class': 'mud-v', 'id': 'mud-usb-role-policy' }, '--') ]),
			E('div', { 'class': 'mud-r' }, [ E('span', { 'class': 'mud-k' }, _('Data role now')), E('span', { 'class': 'mud-v', 'id': 'mud-usb-role-now' }, '--') ]),
			E('div', { 'class': 'mud-r' }, [ E('span', { 'class': 'mud-k' }, _('Host capable')), E('span', { 'class': 'mud-v', 'id': 'mud-usb-role-capable' }, '--') ]),
			E('div', { 'class': 'mud-device-actions' }, [
				E('button', { 'class': 'mud-btn', 'id': 'mud-usb-role-auto' }, _('Automatic')),
				E('button', { 'class': 'mud-btn', 'id': 'mud-usb-role-host' }, _('Host')),
				E('button', { 'class': 'mud-btn', 'id': 'mud-usb-role-device' }, _('Device'))
			]),
			E('div', { 'class': 'mud-note' }, _('Host powers a USB stick, keyboard or network adapter on an OTG adapter. It undoes itself after 60 seconds unless kept from a shell, because the port is also the way to the computer; asking for it replugs the USB link for a few seconds. Automatic and Device keep the link.')),
			E('div', { 'class': 'mud-note', 'id': 'mud-usb-role-msg', 'role': 'status', 'aria-live': 'polite' })
		]);
		root.insertBefore(card, before || null);
		this.card = card;
		this.wire();
		this.refresh();
		return card;
	},
	q: function(id) { return this.card.querySelector('#mud-usb-role-' + id); },
	word: function(v) {
		switch (v) {
		case 'auto': return _('Automatic');
		case 'host': return _('Host');
		case 'device': return _('Device');
		default: return _('Unavailable');
		}
	},
	paint: function(s) {
		var self = this, can = s.ok && s.policy !== 'unavailable';
		this.q('policy').textContent = s.ok ? this.word(s.policy) : _('Unavailable');
		this.q('now').textContent = s.ok ? this.word(s.role) : '--';
		this.q('capable').textContent = s.ok ? (s.host_capable ? _('Yes') : _('No')) : '--';
		[ 'auto', 'host', 'device' ].forEach(function(v) { self.q(v).disabled = !can || s.policy === v; });
	},
	refresh: function() {
		var self = this;
		return L.resolveDefault(callGet(), {}).then(function(s) { self.paint(s || {}); });
	},
	apply: function(value) {
		var self = this, msg = this.q('msg');
		[ 'auto', 'host', 'device' ].forEach(function(id) { self.q(id).disabled = true; });
		msg.textContent = _('Working…');
		return L.resolveDefault(callSet('role', value), {}).then(function(r) {
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
		[ 'auto', 'host', 'device' ].forEach(function(v) { self.q(v).addEventListener('click', function() { self.apply(v); }); });
	}
});
