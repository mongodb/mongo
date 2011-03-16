# Data for config.py, describes all configuration key / value pairs

class LogRecordType:
	def __init__(self, name, fields):
		self.name = name
		self.fields = fields

types = [
	LogRecordType('debug', [('string', 'message')])
]
