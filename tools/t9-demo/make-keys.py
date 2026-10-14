#!/usr/bin/env python3
# Writes key scripts for t9-replay (KEY HOLD_MS AFTER_MS per line).
#
#   make-keys.py message   demo.keys   the message/password demo (default pace)
#   make-keys.py shell     shell.keys  shell, nano and xxd at a quicker
#                                      pace (run s22-t9d with -t 450 -l 350)
#
# type_text() turns text into key presses the way a person would on the
# keypad, tracking s22-t9d's mode. Tokens: {ENTER} {TAB} {ESC} {BS}
# {CTRL-x} {PAUSE n}.
import re
import sys

TAPS = {'0': ' 0', '1': ".,?!-/_:;'\"1", '2': 'abc2', '3': 'def3', '4': 'ghi4',
	'5': 'jkl5', '6': 'mno6', '7': 'pqrs7', '8': 'tuv8', '9': 'wxyz9',
	'*': '*+=@#$%&|~`^\\()[]{}<>'}
MODES = ['abc', 'Abc', 'ABC', '123']


class Typist:
	def __init__(self, tap, same, nxt, wait, hold):
		self.tap, self.same, self.next, self.wait, self.hold = tap, same, nxt, wait, hold
		self.seq = []
		self.mode = 'abc'
		self.last_key = None		# a multi-tap key whose character may still be pending

	def press(self, key, hold=None, after=None):
		self.seq.append((key, hold or self.tap, after or self.next))

	def pause(self, ms):
		if not self.seq:		# t9-replay's START_MS covers the start
			return
		k, h, a = self.seq[-1]
		self.seq[-1] = (k, h, a + ms)

	def set_mode(self, want):
		while self.mode != want:
			self.press('#')
			self.mode = MODES[(MODES.index(self.mode) + 1) % 4]
		self.last_key = None

	def taps(self, key, n):
		if self.last_key == key:	# wait out the tap window first
			self.pause(self.wait)
		for i in range(n):
			self.press(key, after=self.same if i < n - 1 else self.next)
		self.last_key = key

	def char(self, c):
		if c.isdigit():
			if self.mode == '123':
				self.press(c)
			else:
				self.press(c, hold=self.hold)
			self.last_key = None
			return
		if c.isalpha():
			want = 'abc' if c.islower() else 'Abc'
			if self.mode != want:
				self.set_mode(want)
			c = c.lower()
		for key, chars in TAPS.items():
			if c in chars and (key != '*' or c not in TAPS['1']):
				self.taps(key, chars.index(c) + 1)
				break
		else:
			raise ValueError(f'cannot type {c!r}')
		if self.mode == 'Abc' and c.isalpha():
			self.mode = 'abc'	# s22-t9d drops back after one capital

	def text(self, s):
		for tok in re.split(r'(\{[^}]+\})', s):
			if not tok:
				continue
			if tok.startswith('{'):
				name = tok[1:-1]
				self.last_key = None
				if name == 'ENTER': self.press('OK')
				elif name == 'TAB': self.press('Home')
				elif name == 'ESC': self.press('Back')
				elif name == 'BS': self.press('C')
				elif name.startswith('CTRL-'):
					self.press('Recents')
					self.char(name[5:])
					self.pause(self.wait)	# held back until final
				elif name.startswith('PAUSE '): self.pause(int(name[6:]))
				else: raise ValueError(tok)
			else:
				for c in tok:
					self.char(c)

	def write(self, path):
		with open(path, 'w') as f:
			f.write('-- written by make-keys.py: KEY HOLD_MS AFTER_MS\n')
			for k, h, a in self.seq:
				f.write(f'{k} {h} {a}\n')
		total = sum(h + a for _, h, a in self.seq) / 1000
		print(f'{path}: {len(self.seq)} presses, {total:.1f} s')


def message():
	t = Typist(tap=90, same=230, nxt=380, wait=1150, hold=750)
	t.text('Hello, world! ')
	t.set_mode('ABC')
	for c in 'LINUX':
		t.taps(*[(k, v.index(c.lower()) + 1) for k, v in TAPS.items() if c.lower() in v][0])
	t.text(' ')
	t.set_mode('123')
	t.text('2026')
	t.set_mode('abc')
	t.text('{PAUSE 700}{ENTER}{PAUSE 1100}hunter2{PAUSE 400}{ENTER}{PAUSE 2500}')
	t.write('demo.keys')


def shell():
	t = Typist(tap=50, same=110, nxt=170, wait=520, hold=420)
	t.text('{PAUSE 600}ls /{ENTER}{PAUSE 1200}')
	t.text('cd /tmp{ENTER}{PAUSE 600}')
	t.text('nano hi.txt{ENTER}{PAUSE 1200}')
	t.text('Hello from a flip phone.{ENTER}typed with T9, 12 keys{PAUSE 900}')
	t.text('{CTRL-o}{PAUSE 700}{ENTER}{PAUSE 900}{CTRL-x}{PAUSE 900}')
	t.text('clear{ENTER}{PAUSE 500}')
	t.text('cat hi.txt{ENTER}{PAUSE 1300}')
	t.text('xxd -c 8 hi{TAB}{PAUSE 400}{ENTER}{PAUSE 3500}')
	t.write('shell.keys')


if __name__ == '__main__':
	{'message': message, 'shell': shell}[sys.argv[1] if len(sys.argv) > 1 else 'message']()
