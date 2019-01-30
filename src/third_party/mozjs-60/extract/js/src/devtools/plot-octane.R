#!/usr/bin/env Rscript

# Usage:
#
#     octane.R control.csv variable.csv
#
# Output will be placed in Rplots.pdf
#
# Remember: on Octane, higher is better!

library(ggplot2)

args <- commandArgs(trailingOnly = TRUE)

# Reading in data.
control <- read.table(args[1], sep=",", header=TRUE)
variable <- read.table(args[2], sep=",", header=TRUE)

# Pulling out columns that we want to plot.
# Not totally necessary.
ctrl <- control$Score..version.9.
var <- variable$Score..version.9.

# Concatenating the values we want to plot.
score <- c(ctrl, var)
# Creating a vector of labels for the data points.
label <- c(rep("control", length(ctrl)), rep("variable", length(var)))

# Creating a data frame of the score and label.
data <- data.frame(label, score)

# Now plotting!
ggplot(data, aes(label, score, color=label, pch=label)) +
  # Adding boxplot without the outliers.
  geom_boxplot(outlier.shape=NA) +
  # Adding jitter plot on top of the boxplot. If you want to spread the points
  # more, increase jitter.
  geom_jitter(position=position_jitter(width=0.05)) 
