class PerfStat:
    def __init__(self,
                 short_label: str,
                 pattern: str,
                 input_offset: int,
                 output_label: str,
                 output_precision: int = 0,
                 conversion_function=int):
        self.short_label: str = short_label
        self.pattern: str = pattern
        self.input_offset: int = input_offset
        self.output_label: str = output_label
        self.output_precision: int = output_precision
        self.conversion_function = conversion_function
        self.values = []

    def add_value(self, value):
        converted_value = self.conversion_function(value)
        self.values.append(converted_value)

    def get_num_values(self):
        return len(self.values)

    def get_average(self):
        num_values = len(self.values)
        total = sum(self.values)
        average = self.conversion_function(total / num_values)
        return average

    def get_skipminmax_average(self):
        num_values = len(self.values)
        assert num_values >= 3
        minimum = min(self.values)
        maximum = max(self.values)
        total = sum(self.values)
        total_skipminmax = total - maximum - minimum
        num_values_skipminmax = num_values - 2
        skipminmax_average = self.conversion_function(total_skipminmax / num_values_skipminmax)
        return skipminmax_average

    def get_core_average(self):
        if len(self.values) >= 3:
            return self.get_skipminmax_average()
        else:
            return self.get_average()
