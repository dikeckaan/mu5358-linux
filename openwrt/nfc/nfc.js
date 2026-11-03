'use strict';
'require baseclass';
'require rpc';
'require ui';

/* The "NFC" card on System > Device (mu5358/nfc, loaded by device.js through customize.sh): the device's NFC tag
 * carries the hotspot's Wi-Fi credential, a phone that touches the device joins the hotspot. Talks to the rpcd object
 * mu5358nfc (openwrt/nfc/rpcd-mu5358nfc), which runs mu5358-nfc-sync on|off. The card never shows the network's name
 * or key: only what kind of record the tag holds. */
var callGet = rpc.declare({ object: 'mu5358nfc', method: 'get', expect: { '': {} }, nobatch: true });
var callSet = rpc.declare({ object: 'mu5358nfc', method: 'set', params: [ 'value' ], expect: { '': {} }, nobatch: true });

return baseclass.extend({
	render: function(root, before) {
		var card = E('section', { 'class': 'mud-card', 'id': 'mud-nfc-card' }, [
			E('h3', {}, _('NFC tap-to-connect')),
			E('div', { 'class': 'mud-r' }, [ E('span', { 'class': 'mud-k' }, _('State')), E('span', { 'class': 'mud-v', 'id': 'mud-nfc-state' }, '--') ]),
			E('div', { 'class': 'mud-r' }, [ E('span', { 'class': 'mud-k' }, _('Tag')), E('span', { 'class': 'mud-v', 'id': 'mud-nfc-tag' }, '--') ]),
			E('div', { 'class': 'mud-device-actions' }, [
				E('button', { 'class': 'mud-btn', 'id': 'mud-nfc-on' }, _('Turn on')),
				E('button', { 'class': 'mud-btn', 'id': 'mud-nfc-off' }, _('Turn off'))
			]),
			E('div', { 'class': 'mud-note' }, _('A phone held to the back of the device reads the name and key of the hotspot from the NFC tag and joins. The tag follows the hotspot: its name, key, encryption and on/off. Off leaves an empty tag.')),
			E('div', { 'class': 'mud-note', 'id': 'mud-nfc-msg', 'role': 'status', 'aria-live': 'polite' })
		]);
		root.insertBefore(card, before || null);
		this.card = card;
		this.wire();
		this.refresh();
		return card;
	},
	q: function(id) { return this.card.querySelector('#mud-nfc-' + id); },
	tagText: function(s) {
		switch (s.result) {
		case 'open': return _('Hotspot credential (open network)');
		case 'wpa2': return _('Hotspot credential (WPA2)');
		case 'disabled': return _('Empty (NFC is off)');
		case 'hotspot_off': return _('Empty (the hotspot is off)');
		case 'unsupported': return _('Empty (phones take WPA2 or WPA2/WPA3 mixed from a tag; the hotspot uses something else)');
		case 'no_tag': return _('No tag reachable');
		case 'error': return _('The last update failed');
		default: return '--';
		}
	},
	paint: function(s) {
		var on = s.state !== 'off', can = s.ok && s.supported;
		this.q('state').textContent = !s.ok ? _('Unavailable') : !s.supported ? _('Unsupported') : on ? _('On') : _('Off');
		this.q('tag').textContent = s.ok ? this.tagText(s) : '--';
		this.q('on').disabled = !can || on;
		this.q('off').disabled = !can || !on;
	},
	refresh: function() {
		var self = this;
		return L.resolveDefault(callGet(), {}).then(function(s) { self.paint(s || {}); });
	},
	apply: function(value) {
		var self = this, msg = this.q('msg');
		[ 'on', 'off' ].forEach(function(id) { self.q(id).disabled = true; });
		msg.textContent = _('Working…');
		return L.resolveDefault(callSet(value), {}).then(function(r) {
			msg.textContent = '';
			if (!r || !r.ok)
				ui.addNotification(null, E('p', {}, r && r.error === 'nfc_failed' ? _('The tag was not updated; the hotspot is unchanged') : _('The NFC adapter gave no valid reply')), 'danger');
			return self.refresh();
		});
	},
	wire: function() {
		var self = this;
		this.q('on').addEventListener('click', function() { self.apply('on'); });
		this.q('off').addEventListener('click', function() { self.apply('off'); });
	}
});
